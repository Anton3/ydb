#include "kqp_workload_service_ut_common.h"

#include <ydb/core/base/backtrace.h>
#include <ydb/core/base/counters.h>

#include <ydb/core/kqp/common/events/events.h>
#include <ydb/core/kqp/common/simple/services.h>
#include <ydb/core/kqp/executer_actor/kqp_executer.h>
#include <ydb/core/kqp/workload_service/actors/actors.h>
#include <ydb/core/kqp/workload_service/tables/table_queries.h>

#include <ydb/core/kqp/ut/common/kqp_ut_common.h>


namespace NKikimr::NKqp::NWorkload {

namespace {

using namespace NActors;
using namespace NKikimrConfig;
using namespace NThreading;
using namespace NYdb;
using namespace Tests;


// Events

struct TEvQueryRunner {
    // Event ids
    enum EEv : ui32 {
        EvExecutionStarted = EventSpaceBegin(TEvents::ES_PRIVATE),
        EvContinueExecution,
        EvExecutionFinished,

        EvEnd
    };

    static_assert(EvEnd < EventSpaceEnd(TEvents::ES_PRIVATE), "expect EvEnd < EventSpaceEnd(TEvents::ES_PRIVATE)");

    // Events
    struct TEvExecutionStarted : public TEventLocal<TEvExecutionStarted, EvExecutionStarted> {
    };

    struct TEvContinueExecution : public TEventLocal<TEvContinueExecution, EvContinueExecution> {
        explicit TEvContinueExecution(TPromise<void> promise)
            : Promise(promise)
        {}

        TPromise<void> Promise;
    };

    struct TEvExecutionFinished : public TEventLocal<TEvExecutionFinished, EvExecutionFinished> {
    };
};

// Query runner

class TQueryRunnerActor : public TActorBootstrapped<TQueryRunnerActor> {
    using TBase = TActorBootstrapped<TQueryRunnerActor>;

public:
    TQueryRunnerActor(std::unique_ptr<TEvKqp::TEvQueryRequest> request, TPromise<TQueryRunnerResult> promise, const TQueryRunnerSettings& settings, ui32 targetNodeId)
        : Request_(std::move(request))
        , Promise_(promise)
        , Settings_(settings)
        , TargetNodeId_(targetNodeId)
    {}

    void Registered(TActorSystem* sys, const TActorId& owner) override {
        TBase::Registered(sys, owner);
        Owner = owner;
    }

    void Bootstrap() {
        ActorIdToProto(SelfId(), Request_->Record.MutableRequestActorId());
        Send(MakeKqpProxyID(TargetNodeId_), std::move(Request_));

        Become(&TQueryRunnerActor::StateFunc);
    }

    void Handle(TEvKqpExecuter::TEvStreamData::TPtr& ev) {
        UNIT_ASSERT_C(Settings_.ExecutionExpected_ || ExecutionContinued, "Unexpected stream data, execution is not expected and was not continued");
        if (!ExecutionStartReported) {
            ExecutionStartReported = true;
            SendNotification<TEvQueryRunner::TEvExecutionStarted>();
        }

        auto response = std::make_unique<TEvKqpExecuter::TEvStreamDataAck>();
        response->Record.SetSeqNo(ev->Get()->Record.GetSeqNo());
        response->Record.SetFreeSpace(std::numeric_limits<i64>::max());

        auto resultSetIndex = ev->Get()->Record.GetQueryResultIndex();
        if (resultSetIndex >= Result_.ResultSets.size()) {
            Result_.ResultSets.resize(resultSetIndex + 1);
        }

        for (auto& row : *ev->Get()->Record.MutableResultSet()->mutable_rows()) {
            *Result_.ResultSets[resultSetIndex].add_rows() = std::move(row);
        }
        *Result_.ResultSets[resultSetIndex].mutable_columns() = ev->Get()->Record.GetResultSet().columns();

        if (!Settings_.HangUpDuringExecution_ || ExecutionContinued) {
            Send(ev->Sender, response.release());
        } else {
            DelayedAckQueue.emplace(new IEventHandle(ev->Sender, SelfId(), response.release()));
        }
    }

    void Handle(TEvKqp::TEvQueryResponse::TPtr& ev) {
        SendNotification<TEvQueryRunner::TEvExecutionFinished>();

        Result_.Response = ev->Get()->Record.GetRef();
        Promise_.SetValue(Result_);
        PassAway();
    }

    void Handle(TEvQueryRunner::TEvContinueExecution::TPtr& ev) {
        UNIT_ASSERT_C(!ExecutionContinued, "Got second continue execution event");
        ev->Get()->Promise.SetValue();

        ExecutionContinued = true;
        while (!DelayedAckQueue.empty()) {
            Send(DelayedAckQueue.front().release());
            DelayedAckQueue.pop();
        }
    }

    STRICT_STFUNC(StateFunc,
        hFunc(TEvKqpExecuter::TEvStreamData, Handle);
        hFunc(TEvKqp::TEvQueryResponse, Handle);
        hFunc(TEvQueryRunner::TEvContinueExecution, Handle);
    )

private:
    template <typename TEvent>
    void SendNotification() const {
        Send(Owner, new TEvent());
        if (Settings_.InFlightCoordinatorActorId_) {
            Send(*Settings_.InFlightCoordinatorActorId_, new TEvent());
        }
    }

private:
    std::unique_ptr<TEvKqp::TEvQueryRequest> Request_;
    TPromise<TQueryRunnerResult> Promise_;
    const TQueryRunnerSettings Settings_;
    const ui32 TargetNodeId_;
    TActorId Owner;

    TQueryRunnerResult Result_;
    std::queue<std::unique_ptr<IEventHandle>> DelayedAckQueue;
    bool ExecutionStartReported = false;
    bool ExecutionContinued = false;
};

// In flight coordinator

class TInFlightCoordinatorActor : public TActor<TInFlightCoordinatorActor> {
    using TBase = TActor<TInFlightCoordinatorActor>;

public:
    TInFlightCoordinatorActor(ui32 numberRequests, ui32 expectedInFlight, ui32 nodeCount)
        : TBase(&TInFlightCoordinatorActor::StateFunc)
        , ExpectedInFlight(expectedInFlight)
        , NodeCount(nodeCount)
        , RequestsRemains(numberRequests)
    {
        UNIT_ASSERT_C(RequestsRemains > 0, "At least one request should be started");
        PendingRequests.reserve(expectedInFlight);
        RunningRequests.reserve(expectedInFlight);
    }

    void Handle(TEvQueryRunner::TEvExecutionStarted::TPtr& ev) {
        const auto& runnerId = ev->Sender;
        UNIT_ASSERT_C(!PendingRequests.contains(runnerId) && !RunningRequests.contains(runnerId), "Unexpected InFlightCoordinator state");

        PendingRequests.insert(runnerId);
        UNIT_ASSERT_LE_C(PendingRequests.size(), ExpectedInFlight + NodeCount - 1, "Too many in flight requests");
        TryStartNextChunk();
    }

    void Handle(TEvQueryRunner::TEvExecutionFinished::TPtr& ev) {
        const auto& runnerId = ev->Sender;
        UNIT_ASSERT_C(RunningRequests.contains(runnerId), "Unexpected InFlightCoordinator state");

        RunningRequests.erase(runnerId);
        TryStartNextChunk();

        --RequestsRemains;
        if (!RequestsRemains) {
            UNIT_ASSERT_C(PendingRequests.size() + RunningRequests.size() == 0, "To many requests started");
            PassAway();
        }
    }

    STRICT_STFUNC(StateFunc,
        hFunc(TEvQueryRunner::TEvExecutionStarted, Handle);
        hFunc(TEvQueryRunner::TEvExecutionFinished, Handle);
    )

private:
    void TryStartNextChunk() {
        if (!RunningRequests.empty() || PendingRequests.size() < std::min(ExpectedInFlight, RequestsRemains)) {
            return;
        }

        while (!PendingRequests.empty()) {
            const auto runnerId = *PendingRequests.begin();
            PendingRequests.erase(runnerId);

            Send(runnerId, new TEvQueryRunner::TEvContinueExecution(NewPromise()));
            RunningRequests.insert(runnerId);
        }
    }

private:
    const ui32 ExpectedInFlight;
    const ui32 NodeCount;

    ui32 RequestsRemains = 0;
    std::unordered_set<TActorId> PendingRequests;
    std::unordered_set<TActorId> RunningRequests;
};

// Ydb setup

class TWorkloadServiceYdbSetup : public IYdbSetup {
private:
    TAppConfig GetAppConfig() const {
        TAppConfig appConfig;
        appConfig.MutableFeatureFlags()->SetEnableResourcePools(Settings_.EnableResourcePools_);

        return appConfig;
    }

    void SetLoggerSettings(TServerSettings& serverSettings) const {
        auto loggerInitializer = [](TTestActorRuntime& runtime) {
            runtime.SetLogPriority(NKikimrServices::KQP_WORKLOAD_SERVICE, NLog::EPriority::PRI_TRACE);
            runtime.SetLogPriority(NKikimrServices::KQP_SESSION, NLog::EPriority::PRI_DEBUG);
        };

        serverSettings.SetLoggerInitializer(loggerInitializer);
    }

    TServerSettings GetServerSettings(ui32 grpcPort) {
        ui32 msgBusPort = PortManager_.GetPort();
        TAppConfig appConfig = GetAppConfig();

        auto serverSettings = TServerSettings(msgBusPort)
            .SetGrpcPort(grpcPort)
            .SetNodeCount(Settings_.NodeCount_)
            .SetDomainName(Settings_.DomainName_)
            .SetAppConfig(appConfig)
            .SetFeatureFlags(appConfig.GetFeatureFlags());

        SetLoggerSettings(serverSettings);

        return serverSettings;
    }

    void InitializeServer() {
        ui32 grpcPort = PortManager_.GetPort();
        TServerSettings serverSettings = GetServerSettings(grpcPort);

        Server_ = std::make_unique<TServer>(serverSettings);
        Server_->EnableGRpc(grpcPort);
        GetRuntime()->SetDispatchTimeout(FUTURE_WAIT_TIMEOUT);

        Client_ = std::make_unique<TClient>(serverSettings);
        Client_->InitRootScheme();

        YdbDriver_ = std::make_unique<TDriver>(TDriverConfig()
            .SetEndpoint(TStringBuilder() << "localhost:" << grpcPort)
            .SetDatabase(TStringBuilder() << "/" << Settings_.DomainName_));

        TableClient_ = std::make_unique<NYdb::NTable::TTableClient>(*YdbDriver_, NYdb::NTable::TClientSettings().AuthToken("user@" BUILTIN_SYSTEM_DOMAIN));
        TableClientSession_ = std::make_unique<NYdb::NTable::TSession>(TableClient_->CreateSession().GetValueSync().GetSession());
    }

    void CreateSamplePool() const {
        if (!Settings_.EnableResourcePools_) {
            return;
        }

        NResourcePool::TPoolSettings poolConfig;
        poolConfig.ConcurrentQueryLimit = Settings_.ConcurrentQueryLimit_;
        poolConfig.QueueSize = Settings_.QueueSize_;
        poolConfig.QueryCancelAfter = Settings_.QueryCancelAfter_;
        poolConfig.QueryMemoryLimitPercentPerNode = Settings_.QueryMemoryLimitPercentPerNode_;

        TActorId edgeActor = GetRuntime()->AllocateEdgeActor();
        GetRuntime()->Register(CreatePoolCreatorActor(edgeActor, Settings_.DomainName_, Settings_.PoolId_, poolConfig, nullptr, {}));
        auto response = GetRuntime()->GrabEdgeEvent<TEvPrivate::TEvCreatePoolResponse>(edgeActor, FUTURE_WAIT_TIMEOUT);
        UNIT_ASSERT_VALUES_EQUAL_C(response->Get()->Status, Ydb::StatusIds::SUCCESS, response->Get()->Issues.ToOneLineString());
    }

public:
    explicit TWorkloadServiceYdbSetup(const TYdbSetupSettings& settings)
        : Settings_(settings)
    {
        EnableYDBBacktraceFormat();
        InitializeServer();
        CreateSamplePool();
    }

    // Scheme queries helpers
    NYdb::NScheme::TSchemeClient GetSchemeClient() const override {
        return NYdb::NScheme::TSchemeClient(*YdbDriver_);
    }

    void ExecuteSchemeQuery(const TString& query, NYdb::EStatus expectedStatus = NYdb::EStatus::SUCCESS, const TString& expectedMessage = "") const override {
        TStatus status = TableClientSession_->ExecuteSchemeQuery(query).GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(status.GetStatus(), expectedStatus, status.GetIssues().ToOneLineString());
        if (expectedStatus != NYdb::EStatus::SUCCESS) {
            UNIT_ASSERT_STRING_CONTAINS(status.GetIssues().ToString(), expectedMessage);
        }
    }

    THolder<NKikimr::NSchemeCache::TSchemeCacheNavigate> Navigate(const TString& path, NKikimr::NSchemeCache::TSchemeCacheNavigate::EOp operation = NSchemeCache::TSchemeCacheNavigate::EOp::OpUnknown) const override {
        return NKqp::Navigate(*GetRuntime(), GetRuntime()->AllocateEdgeActor(), CanonizePath({Settings_.DomainName_, path}), operation);
    }

    void WaitPoolAccess(const TString& userSID, ui32 access, const TString& poolId = "") const override {
        auto token = NACLib::TUserToken(userSID, {});

        TInstant start = TInstant::Now();
        while (TInstant::Now() - start <= FUTURE_WAIT_TIMEOUT) {
            if (auto response = Navigate(TStringBuilder() << ".resource_pools/" << (poolId ? poolId : Settings_.PoolId_))) {
                const auto& result = response->ResultSet.at(0);
                bool resourcePool = result.Kind == NSchemeCache::TSchemeCacheNavigate::EKind::KindResourcePool;
                if (resourcePool && (!result.SecurityObject || result.SecurityObject->CheckAccess(access, token))) {
                    return;
                }
                Cerr << "WaitPoolAccess " << TInstant::Now() - start << ": " << (resourcePool ? TStringBuilder() << "access denied" : TStringBuilder() << "unexpected kind " << result.Kind) << "\n";
            } else {
                Cerr << "WaitPoolAccess " << TInstant::Now() - start << ": empty response\n";
            }
            Sleep(TDuration::Seconds(1));
        }
        UNIT_ASSERT_C(false, "Pool version waiting timeout");
    }

    // Generic query helpers
    TQueryRunnerResult ExecuteQuery(const TString& query, TQueryRunnerSettings settings = TQueryRunnerSettings()) const override {
        SetupDefaultSettings(settings, false);

        auto event = GetQueryRequest(query, settings);
        auto promise = NewPromise<TQueryRunnerResult>();
        GetRuntime()->Register(new TQueryRunnerActor(std::move(event), promise, settings, GetRuntime()->GetNodeId(settings.NodeIndex_)));

        return promise.GetFuture().GetValue(FUTURE_WAIT_TIMEOUT);
    }

    TQueryRunnerResultAsync ExecuteQueryAsync(const TString& query, TQueryRunnerSettings settings) const override {
        SetupDefaultSettings(settings, true);

        auto event = GetQueryRequest(query, settings);
        auto promise = NewPromise<TQueryRunnerResult>();
        auto edgeActor = GetRuntime()->AllocateEdgeActor();
        auto runerActor = GetRuntime()->Register(new TQueryRunnerActor(std::move(event), promise, settings, GetRuntime()->GetNodeId(settings.NodeIndex_)), 0, 0, TMailboxType::Simple, 0, edgeActor);

        return {.AsyncResult = promise.GetFuture(), .QueryRunnerActor = runerActor, .EdgeActor = edgeActor};
    }

    // Async query execution actions
    void WaitQueryExecution(const TQueryRunnerResultAsync& query, TDuration timeout = FUTURE_WAIT_TIMEOUT) const override {
        auto event = GetRuntime()->GrabEdgeEvent<TEvQueryRunner::TEvExecutionStarted>(query.EdgeActor, timeout);
        UNIT_ASSERT_C(event, "WaitQueryExecution timeout");
    }

    void ContinueQueryExecution(const TQueryRunnerResultAsync& query) const override {
        auto promise = NewPromise();
        GetRuntime()->Send(query.QueryRunnerActor, query.EdgeActor, new TEvQueryRunner::TEvContinueExecution(promise));
        promise.GetFuture().GetValue(FUTURE_WAIT_TIMEOUT);
    }

    TActorId CreateInFlightCoordinator(ui32 numberRequests, ui32 expectedInFlight) const override {
        return GetRuntime()->Register(new TInFlightCoordinatorActor(numberRequests, expectedInFlight, Settings_.NodeCount_));
    }

    // Pools actions
    TPoolStateDescription GetPoolDescription(TDuration leaseDuration = FUTURE_WAIT_TIMEOUT, const TString& poolId = "") const override {
        const auto& edgeActor = GetRuntime()->AllocateEdgeActor();

        GetRuntime()->Register(CreateRefreshPoolStateActor(edgeActor, Settings_.DomainName_, poolId ? poolId : Settings_.PoolId_, leaseDuration, GetRuntime()->GetAppData().Counters));
        auto response = GetRuntime()->GrabEdgeEvent<TEvPrivate::TEvRefreshPoolStateResponse>(edgeActor, FUTURE_WAIT_TIMEOUT);
        UNIT_ASSERT_VALUES_EQUAL_C(response->Get()->Status, Ydb::StatusIds::SUCCESS, response->Get()->Issues.ToOneLineString());

        return response->Get()->PoolState;
    }

    void WaitPoolState(const TPoolStateDescription& state, const TString& poolId = "") const override {
        TInstant start = TInstant::Now();
        while (TInstant::Now() - start <= FUTURE_WAIT_TIMEOUT) {
            auto description = GetPoolDescription(TDuration::Zero(), poolId);
            if (description.DelayedRequests == state.DelayedRequests && description.RunningRequests == state.RunningRequests) {
                return;
            }

            Cerr << "WaitPoolState " << TInstant::Now() - start << ": delayed = " << description.DelayedRequests << ", running = " << description.RunningRequests << "\n";
            Sleep(TDuration::Seconds(1));
        }
        UNIT_ASSERT_C(false, "Pool state waiting timeout");
    }

    void WaitPoolHandlersCount(i64 finalCount, std::optional<i64> initialCount = std::nullopt, TDuration timeout = FUTURE_WAIT_TIMEOUT) const override {
        auto counter = GetServiceCounters(GetRuntime()->GetAppData().Counters, "kqp")
            ->GetSubgroup("subsystem", "workload_manager")
            ->GetCounter("ActivePoolHandlers");

        if (initialCount) {
            UNIT_ASSERT_VALUES_EQUAL_C(counter->Val(), *initialCount, "Unexpected pool handlers count");
        }

        TInstant start = TInstant::Now();
        while (TInstant::Now() - start < timeout) {
            if (counter->Val() == finalCount) {
                return;
            }

            Cerr << "WaitPoolHandlersCount " << TInstant::Now() - start << ": number handlers = " << counter->Val() << "\n";
            Sleep(TDuration::Seconds(1));
        }
        UNIT_ASSERT_C(false, "Pool handlers count wait timeout");
    }

    void StopWorkloadService(ui64 nodeIndex = 0) const override {
        GetRuntime()->Send(MakeKqpWorkloadServiceId(GetRuntime()->GetNodeId(nodeIndex)), GetRuntime()->AllocateEdgeActor(), new TEvents::TEvPoison());
        Sleep(TDuration::Seconds(1));
    }

    TTestActorRuntime* GetRuntime() const override {
        return Server_->GetRuntime();
    }

    const TYdbSetupSettings& GetSettings() const override {
        return Settings_;
    }

private:
    void SetupDefaultSettings(TQueryRunnerSettings& settings, bool asyncExecution) const {
        UNIT_ASSERT_C(!settings.HangUpDuringExecution_ || asyncExecution, "Hang up during execution is not supported for sync queries");

        if (!settings.PoolId_) {
            settings.PoolId(Settings_.PoolId_);
        }
    }

    std::unique_ptr<TEvKqp::TEvQueryRequest> GetQueryRequest(const TString& query, const TQueryRunnerSettings& settings) const {
        auto event = std::make_unique<TEvKqp::TEvQueryRequest>();
        event->Record.SetUserToken(NACLib::TUserToken("", settings.UserSID_, {}).SerializeAsString());

        auto request = event->Record.MutableRequest();
        request->SetQuery(query);
        request->SetType(NKikimrKqp::QUERY_TYPE_SQL_GENERIC_QUERY);
        request->SetAction(NKikimrKqp::QUERY_ACTION_EXECUTE);
        request->SetDatabase(Settings_.DomainName_);
        request->SetPoolId(settings.PoolId_);

        return event;
    }

private:
    const TYdbSetupSettings Settings_;

    TPortManager PortManager_;
    std::unique_ptr<TServer> Server_;
    std::unique_ptr<TClient> Client_;
    std::unique_ptr<TDriver> YdbDriver_;

    std::unique_ptr<NYdb::NTable::TTableClient> TableClient_;
    std::unique_ptr<NYdb::NTable::TSession> TableClientSession_;
};

}  // anonymous namespace

//// TQueryRunnerResult

EStatus TQueryRunnerResult::GetStatus() const {
    return static_cast<EStatus>(Response.GetYdbStatus());
}

NYql::TIssues TQueryRunnerResult::GetIssues() const {
    NYql::TIssues issues;
    NYql::IssuesFromMessage(Response.GetResponse().GetQueryIssues(), issues);
    return issues;
}

const Ydb::ResultSet& TQueryRunnerResult::GetResultSet(size_t resultIndex) const {
    UNIT_ASSERT_C(resultIndex < ResultSets.size(), "Invalid result set index");
    return ResultSets[resultIndex];
}

const std::vector<Ydb::ResultSet>& TQueryRunnerResult::GetResultSets() const {
    return ResultSets;
}

//// TQueryRunnerResultAsync

TQueryRunnerResult TQueryRunnerResultAsync::GetResult(TDuration timeout) const {
    return AsyncResult.GetValue(timeout);
}

TFuture<void> TQueryRunnerResultAsync::GetFuture() const {
    return AsyncResult.IgnoreResult();
}

bool TQueryRunnerResultAsync::HasValue() const {
    return AsyncResult.HasValue();
}

//// TYdbSetupSettings

TIntrusivePtr<IYdbSetup> TYdbSetupSettings::Create() const {
    return MakeIntrusive<TWorkloadServiceYdbSetup>(*this);
}

//// TSampleQueriess

void TSampleQueries::CompareYson(const TString& expected, const TString& actual) {
    NKqp::CompareYson(expected, actual);
}

}  // NKikimr::NKqp::NWorkload
