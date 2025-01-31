#include <ydb/core/base/appdata_fwd.h>

#include <ydb/core/kqp/workload_service/ut/common/kqp_workload_service_ut_common.h>


namespace NKikimr::NKqp {

using namespace NWorkload;
using namespace NYdb;


Y_UNIT_TEST_SUITE(KqpWorkloadService) {
    Y_UNIT_TEST(WorkloadServiceDisabledByFeatureFlag) {
        auto ydb = TYdbSetupSettings()
            .EnableResourcePools(false)
            .Create();

        TSampleQueries::TSelect42::CheckResult(ydb->ExecuteQuery(TSampleQueries::TSelect42::Query, TQueryRunnerSettings().PoolId("another_pool_id")));
    }

    TQueryRunnerResultAsync StartQueueSizeCheckRequests(TIntrusivePtr<IYdbSetup> ydb, const TQueryRunnerSettings& settings) {
        // One of these requests should be rejected by QueueSize
        auto firstRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, settings);
        auto secondRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, settings);
        WaitAny(firstRequest.GetFuture(), secondRequest.GetFuture()).GetValue(FUTURE_WAIT_TIMEOUT);

        if (secondRequest.HasValue()) {
            std::swap(firstRequest, secondRequest);
        }
        UNIT_ASSERT_C(firstRequest.HasValue(), "One of two requests shoud be rejected");
        UNIT_ASSERT_C(!secondRequest.HasValue(), "One of two requests shoud be placed in pool");
        TSampleQueries::CheckOverloaded(firstRequest.GetResult(), ydb->GetSettings().PoolId_);

        return secondRequest;
    }

    Y_UNIT_TEST(TestQueueSizeSimple) {
        auto ydb = TYdbSetupSettings()
            .ConcurrentQueryLimit(1)
            .QueueSize(1)
            .Create();

        auto hangingRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, TQueryRunnerSettings().HangUpDuringExecution(true));
        ydb->WaitQueryExecution(hangingRequest);

        auto delayedRequest = StartQueueSizeCheckRequests(ydb, TQueryRunnerSettings().ExecutionExpected(false));

        ydb->ContinueQueryExecution(delayedRequest);
        ydb->ContinueQueryExecution(hangingRequest);
        TSampleQueries::TSelect42::CheckResult(hangingRequest.GetResult());
        TSampleQueries::TSelect42::CheckResult(delayedRequest.GetResult());
    }

    Y_UNIT_TEST(TestQueueSizeManyQueries) {
        auto ydb = TYdbSetupSettings()
            .ConcurrentQueryLimit(1)
            .QueueSize(1)
            .Create();

        auto settings = TQueryRunnerSettings().HangUpDuringExecution(true);
        auto hangingRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, settings);
        ydb->WaitQueryExecution(hangingRequest);

        const ui64 numberRuns = 5;
        for (size_t i = 0; i < numberRuns; ++i) {
            auto delayedRequest = StartQueueSizeCheckRequests(ydb, settings);

            ydb->ContinueQueryExecution(hangingRequest);
            TSampleQueries::TSelect42::CheckResult(hangingRequest.GetResult());

            hangingRequest = delayedRequest;
            ydb->WaitQueryExecution(hangingRequest);
        }

        ydb->ContinueQueryExecution(hangingRequest);
        TSampleQueries::TSelect42::CheckResult(hangingRequest.GetResult());
    }

    Y_UNIT_TEST(TestZeroQueueSize) {
        auto ydb = TYdbSetupSettings()
            .ConcurrentQueryLimit(1)
            .QueueSize(0)
            .Create();

        auto hangingRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, TQueryRunnerSettings().HangUpDuringExecution(true));
        ydb->WaitQueryExecution(hangingRequest);

        TSampleQueries::CheckOverloaded(
            ydb->ExecuteQuery(TSampleQueries::TSelect42::Query, TQueryRunnerSettings().ExecutionExpected(false)),
            ydb->GetSettings().PoolId_
        );

        ydb->ContinueQueryExecution(hangingRequest);
        TSampleQueries::TSelect42::CheckResult(hangingRequest.GetResult());
    }

    Y_UNIT_TEST(TestQueryCancelAfterUnlimitedPool) {
        auto ydb = TYdbSetupSettings()
            .QueryCancelAfter(TDuration::Seconds(10))
            .Create();

        TSampleQueries::CheckCancelled(ydb->ExecuteQueryAsync(
            TSampleQueries::TSelect42::Query,
            TQueryRunnerSettings().HangUpDuringExecution(true)
        ).GetResult());
    }

    Y_UNIT_TEST(TestQueryCancelAfterPoolWithLimits) {
        auto ydb = TYdbSetupSettings()
            .ConcurrentQueryLimit(1)
            .QueueSize(1)
            .QueryCancelAfter(TDuration::Seconds(10))
            .Create();

        auto settings = TQueryRunnerSettings().HangUpDuringExecution(true);
        auto hangingRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, settings);
        ydb->WaitQueryExecution(hangingRequest);        

        auto delayedRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, settings);
        TSampleQueries::CheckCancelled(hangingRequest.GetResult());

        auto result = delayedRequest.GetResult();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::CANCELLED, result.GetIssues().ToString());

        // Check that queue is free
        auto firstRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query);
        auto secondRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query);
        TSampleQueries::TSelect42::CheckResult(firstRequest.GetResult());
        TSampleQueries::TSelect42::CheckResult(secondRequest.GetResult());
    }

    Y_UNIT_TEST(TestStartQueryAfterCancel) {
        const TDuration cancelAfter = TDuration::Seconds(10);
        auto ydb = TYdbSetupSettings()
            .ConcurrentQueryLimit(1)
            .QueryCancelAfter(cancelAfter)
            .Create();

        auto hangingRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, TQueryRunnerSettings().HangUpDuringExecution(true));
        ydb->WaitQueryExecution(hangingRequest);

        Sleep(cancelAfter / 2);

        TSampleQueries::TSelect42::CheckResult(ydb->ExecuteQuery(TSampleQueries::TSelect42::Query));
        TSampleQueries::CheckCancelled(hangingRequest.GetResult());
    }

    Y_UNIT_TEST(TestConcurrentQueryLimit) {
        const ui64 activeCountLimit = 5;
        const ui64 queueSize = 50;
        auto ydb = TYdbSetupSettings()
            .ConcurrentQueryLimit(activeCountLimit)
            .QueueSize(queueSize)
            .QueryCancelAfter(FUTURE_WAIT_TIMEOUT * queueSize)
            .Create();

        auto settings = TQueryRunnerSettings()
            .InFlightCoordinatorActorId(ydb->CreateInFlightCoordinator(queueSize, activeCountLimit))
            .HangUpDuringExecution(true);

        // Initialize queue
        std::vector<TQueryRunnerResultAsync> asyncResults;
        for (size_t i = 0; i < queueSize; ++i) {
            asyncResults.emplace_back(ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, settings));
        }

        for (const auto& asyncResult : asyncResults) {
            TSampleQueries::TSelect42::CheckResult(asyncResult.GetResult());
        }
    }

    Y_UNIT_TEST(TestZeroConcurrentQueryLimit) {
        auto ydb = TYdbSetupSettings()
            .ConcurrentQueryLimit(0)
            .Create();

        auto result = ydb->ExecuteQuery(TSampleQueries::TSelect42::Query);
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::PRECONDITION_FAILED, result.GetIssues().ToString());
        UNIT_ASSERT_STRING_CONTAINS(result.GetIssues().ToString(), TStringBuilder() << "Resource pool " << ydb->GetSettings().PoolId_ << " was disabled due to zero concurrent query limit");
    }

    Y_UNIT_TEST(TestHandlerActorCleanup) {
        auto ydb = TYdbSetupSettings()
            .ConcurrentQueryLimit(1)
            .Create();

        TSampleQueries::TSelect42::CheckResult(ydb->ExecuteQuery(TSampleQueries::TSelect42::Query));
        TSampleQueries::TSelect42::CheckResult(ydb->ExecuteQuery(TSampleQueries::TSelect42::Query, TQueryRunnerSettings().PoolId(NResourcePool::DEFAULT_POOL_ID)));

        ydb->WaitPoolHandlersCount(0, 2, TDuration::Seconds(35));
    }
}

Y_UNIT_TEST_SUITE(KqpWorkloadServiceDistributed) {
    Y_UNIT_TEST(TestDistributedQueue) {
        auto ydb = TYdbSetupSettings()
            .NodeCount(2)
            .ConcurrentQueryLimit(1)
            .QueueSize(1)
            .Create();

        auto hangingRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, TQueryRunnerSettings()
            .HangUpDuringExecution(true)
            .NodeIndex(0)
        );
        ydb->WaitQueryExecution(hangingRequest);

        auto delayedRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, TQueryRunnerSettings()
            .ExecutionExpected(false)
            .NodeIndex(1)
        );
        ydb->WaitPoolState({.DelayedRequests = 1, .RunningRequests = 1});

        // Check distributed queue size
        TSampleQueries::CheckOverloaded(ydb->ExecuteQuery(TSampleQueries::TSelect42::Query, TQueryRunnerSettings().NodeIndex(0)), ydb->GetSettings().PoolId_);

        ydb->ContinueQueryExecution(delayedRequest);
        ydb->ContinueQueryExecution(hangingRequest);
        TSampleQueries::TSelect42::CheckResult(hangingRequest.GetResult());

        // Query should start faster than lease update time
        ydb->WaitQueryExecution(delayedRequest, TDuration::Seconds(5));
        TSampleQueries::TSelect42::CheckResult(delayedRequest.GetResult());
    }

    Y_UNIT_TEST(TestNodeDisconnect) {
        auto ydb = TYdbSetupSettings()
            .NodeCount(2)
            .ConcurrentQueryLimit(1)
            .QueryCancelAfter(TDuration::Minutes(2))
            .Create();

        auto hangingRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, TQueryRunnerSettings()
            .HangUpDuringExecution(true)
            .NodeIndex(0)
        );
        ydb->WaitQueryExecution(hangingRequest);

        auto delayedRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, TQueryRunnerSettings()
            .ExecutionExpected(false)
            .NodeIndex(0)
        );
        ydb->WaitPoolState({.DelayedRequests = 1, .RunningRequests = 1});

        auto request = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, TQueryRunnerSettings()
            .ExecutionExpected(false)
            .NodeIndex(1)
        );
        ydb->WaitPoolState({.DelayedRequests = 2, .RunningRequests = 1});

        ydb->ContinueQueryExecution(request);
        ydb->StopWorkloadService(0);

        // Query should start after lease expiration
        TSampleQueries::TSelect42::CheckResult(request.GetResult(TDuration::Seconds(50)));
    }

    Y_UNIT_TEST(TestDistributedConcurrentQueryLimit) {
        const ui64 nodeCount = 3;
        const ui64 activeCountLimit = 5;
        const ui64 queueSize = 50;
        auto ydb = TYdbSetupSettings()
            .NodeCount(nodeCount)
            .ConcurrentQueryLimit(activeCountLimit)
            .QueueSize(queueSize)
            .QueryCancelAfter(FUTURE_WAIT_TIMEOUT * queueSize)
            .Create();

        auto settings = TQueryRunnerSettings()
            .InFlightCoordinatorActorId(ydb->CreateInFlightCoordinator(queueSize, activeCountLimit))
            .HangUpDuringExecution(true);

        // Initialize queue
        std::vector<TQueryRunnerResultAsync> asyncResults;
        for (size_t i = 0; i < queueSize; ++i) {
            asyncResults.emplace_back(ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, settings.NodeIndex(i % nodeCount)));
        }

        for (const auto& asyncResult : asyncResults) {
            TSampleQueries::TSelect42::CheckResult(asyncResult.GetResult());
        }
    }
}

Y_UNIT_TEST_SUITE(ResourcePoolsDdl) {
    Y_UNIT_TEST(TestCreateResourcePool) {
        auto ydb = TYdbSetupSettings().Create();

        const TString& poolId = "my_pool";
        ydb->ExecuteSchemeQuery(TStringBuilder() << R"(
            CREATE RESOURCE POOL )" << poolId << R"( WITH (
                CONCURRENT_QUERY_LIMIT=1,
                QUEUE_SIZE=0
            );
        )");

        auto hangingRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, TQueryRunnerSettings()
            .HangUpDuringExecution(true)
            .PoolId(poolId)
        );
        ydb->WaitQueryExecution(hangingRequest);

        TSampleQueries::CheckOverloaded(ydb->ExecuteQuery(TSampleQueries::TSelect42::Query, TQueryRunnerSettings().PoolId(poolId)), poolId);

        ydb->ContinueQueryExecution(hangingRequest);
        TSampleQueries::TSelect42::CheckResult(hangingRequest.GetResult());
    }

    Y_UNIT_TEST(TestDefaultPoolRestrictions) {
        auto ydb = TYdbSetupSettings().Create();

        const TString& poolId = NResourcePool::DEFAULT_POOL_ID;
        ydb->ExecuteSchemeQuery(TStringBuilder() << R"(
            CREATE RESOURCE POOL )" << poolId << R"( WITH (
                CONCURRENT_QUERY_LIMIT=0
            );
        )", EStatus::GENERIC_ERROR, "Cannot create default pool manually, pool will be created automatically during first request execution");

        // Create default pool
        TSampleQueries::TSelect42::CheckResult(ydb->ExecuteQuery(TSampleQueries::TSelect42::Query, TQueryRunnerSettings().PoolId(poolId)));

        ydb->ExecuteSchemeQuery(TStringBuilder() << R"(
            ALTER RESOURCE POOL )" << poolId << R"( SET (
                CONCURRENT_QUERY_LIMIT=0
            );
        )", EStatus::GENERIC_ERROR, "Can not change property concurrent_query_limit for default pool");
    }

    Y_UNIT_TEST(TestAlterResourcePool) {
        auto ydb = TYdbSetupSettings()
            .ConcurrentQueryLimit(1)
            .Create();

        auto hangingRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, TQueryRunnerSettings().HangUpDuringExecution(true));
        ydb->WaitQueryExecution(hangingRequest);

        auto delayedRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, TQueryRunnerSettings().ExecutionExpected(false));
        ydb->WaitPoolState({.DelayedRequests = 1, .RunningRequests = 1});

        ydb->ExecuteSchemeQuery(TStringBuilder() << R"(
            ALTER RESOURCE POOL )" << ydb->GetSettings().PoolId_ << R"( SET (
                QUEUE_SIZE=0
            );
        )");
        TSampleQueries::CheckOverloaded(delayedRequest.GetResult(), ydb->GetSettings().PoolId_);

        ydb->ContinueQueryExecution(hangingRequest);
        TSampleQueries::TSelect42::CheckResult(hangingRequest.GetResult());
    }

    Y_UNIT_TEST(TestPoolSwitchToLimitedState) {
        auto ydb = TYdbSetupSettings()
            .Create();

        // Initialize pool
        TSampleQueries::TSelect42::CheckResult(ydb->ExecuteQuery(TSampleQueries::TSelect42::Query));

        // Change pool to limited
        ydb->ExecuteSchemeQuery(TStringBuilder() << R"(
            ALTER RESOURCE POOL )" << ydb->GetSettings().PoolId_ << R"( SET (
                CONCURRENT_QUERY_LIMIT=1
            );
        )");

        // Wait pool change
        TSampleQueries::TSelect42::CheckResult(ydb->ExecuteQuery(TSampleQueries::TSelect42::Query));  // Force pool update
        ydb->WaitPoolHandlersCount(2);

        // Check that pool using tables
        auto hangingRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, TQueryRunnerSettings().HangUpDuringExecution(true));
        ydb->WaitQueryExecution(hangingRequest);
        UNIT_ASSERT_VALUES_EQUAL(ydb->GetPoolDescription().AmountRequests(), 1);

        ydb->ContinueQueryExecution(hangingRequest);
        TSampleQueries::TSelect42::CheckResult(hangingRequest.GetResult());
    }

    Y_UNIT_TEST(TestPoolSwitchToUnlimitedState) {
        auto ydb = TYdbSetupSettings()
            .ConcurrentQueryLimit(1)
            .Create();

        // Initialize pool
        TSampleQueries::TSelect42::CheckResult(ydb->ExecuteQuery(TSampleQueries::TSelect42::Query));

        // Change pool to unlimited
        ydb->ExecuteSchemeQuery(TStringBuilder() << R"(
            ALTER RESOURCE POOL )" << ydb->GetSettings().PoolId_ << R"( RESET (
                CONCURRENT_QUERY_LIMIT
            );
        )");

        // Wait pool change
        TSampleQueries::TSelect42::CheckResult(ydb->ExecuteQuery(TSampleQueries::TSelect42::Query));  // Force pool update
        ydb->WaitPoolHandlersCount(2);

        // Check that pool is not using tables
        auto hangingRequest = ydb->ExecuteQueryAsync(TSampleQueries::TSelect42::Query, TQueryRunnerSettings().HangUpDuringExecution(true));
        ydb->WaitQueryExecution(hangingRequest);
        UNIT_ASSERT_VALUES_EQUAL(ydb->GetPoolDescription().AmountRequests(), 0);

        ydb->ContinueQueryExecution(hangingRequest);
        TSampleQueries::TSelect42::CheckResult(hangingRequest.GetResult());
    }

    Y_UNIT_TEST(TestDropResourcePool) {
        auto ydb = TYdbSetupSettings().Create();

        const TString& poolId = "my_pool";
        ydb->ExecuteSchemeQuery(TStringBuilder() << R"(
            CREATE RESOURCE POOL )" << poolId << R"( WITH (
                CONCURRENT_QUERY_LIMIT=1
            );
        )");

        auto settings = TQueryRunnerSettings().PoolId(poolId);
        TSampleQueries::TSelect42::CheckResult(ydb->ExecuteQuery(TSampleQueries::TSelect42::Query, settings));

        ydb->ExecuteSchemeQuery(TStringBuilder() << R"(
            DROP RESOURCE POOL )" << poolId << ";"
        );

        TInstant start = TInstant::Now();
        while (TInstant::Now() - start <= FUTURE_WAIT_TIMEOUT) {
            if (ydb->Navigate(TStringBuilder() << ".resource_pools/" << poolId)->ResultSet.at(0).Kind == NSchemeCache::TSchemeCacheNavigate::EKind::KindUnknown) {
                auto result = ydb->ExecuteQuery(TSampleQueries::TSelect42::Query, settings);
                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::NOT_FOUND, result.GetIssues().ToString());
                UNIT_ASSERT_STRING_CONTAINS(result.GetIssues().ToString(), TStringBuilder() << "Resource pool " << poolId << " not found");
                return;
            }

            Cerr << "WaitPoolDrop " << TInstant::Now() - start << "\n";
            Sleep(TDuration::Seconds(1));
        }
        UNIT_ASSERT_C(false, "Pool drop waiting timeout");
    }

    Y_UNIT_TEST(TestResourcePoolAcl) {
        auto ydb = TYdbSetupSettings().Create();

        const TString& poolId = "my_pool";
        const TString& userSID = "user@test";
        ydb->ExecuteSchemeQuery(TStringBuilder() << R"(
            CREATE RESOURCE POOL )" << poolId << R"( WITH (
                CONCURRENT_QUERY_LIMIT=1
            );
            GRANT DESCRIBE SCHEMA ON `/Root/.resource_pools/)" << poolId << "` TO `" << userSID << "`;"
        );
        ydb->WaitPoolAccess(userSID, NACLib::EAccessRights::DescribeSchema, poolId);

        auto settings = TQueryRunnerSettings().PoolId(poolId).UserSID(userSID);
        auto result = ydb->ExecuteQuery(TSampleQueries::TSelect42::Query, settings);
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::UNAUTHORIZED, result.GetIssues().ToString());
        UNIT_ASSERT_STRING_CONTAINS(result.GetIssues().ToString(), TStringBuilder() << "You don't have access permissions for resource pool " << poolId);

        ydb->ExecuteSchemeQuery(TStringBuilder() << R"(
            GRANT SELECT ROW ON `/Root/.resource_pools/)" << poolId << "` TO `" << userSID << "`;"
        );
        ydb->WaitPoolAccess(userSID, NACLib::EAccessRights::SelectRow, poolId);
        TSampleQueries::TSelect42::CheckResult(ydb->ExecuteQuery(TSampleQueries::TSelect42::Query, settings));
    }
}

}  // namespace NKikimr::NKqp
