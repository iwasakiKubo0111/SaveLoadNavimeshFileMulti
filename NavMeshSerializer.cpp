#include "NavMeshSerializer.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "AI/NavDataGenerator.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"

ANavMeshSerializer::ANavMeshSerializer()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
}

//======================================================================//
// ユーティリティ
//======================================================================//

FName ANavMeshSerializer::GetAgentNameFromNavMesh(const ARecastNavMesh* NavMesh)
{
    if (!NavMesh)
    {
        return NAME_None;
    }
    const FNavDataConfig& Config = NavMesh->GetConfig();
    return Config.Name;
}

TArray<ARecastNavMesh*> ANavMeshSerializer::GetAllRecastNavMeshes() const
{
    TArray<ARecastNavMesh*> Result;

    UWorld* World = GetWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: World is null"));
        return Result;
    }

    for (TActorIterator<ARecastNavMesh> It(World); It; ++It)
    {
        ARecastNavMesh* NavMesh = *It;
        if (NavMesh && !NavMesh->IsPendingKillPending())
        {
            Result.Add(NavMesh);
        }
    }

    if (Result.Num() == 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("NavMeshSerializer: No RecastNavMesh found in world"));
    }

    return Result;
}

ARecastNavMesh* ANavMeshSerializer::FindNavMeshByAgentName(FName AgentName) const
{
    UWorld* World = GetWorld();
    if (!World) return nullptr;

    for (TActorIterator<ARecastNavMesh> It(World); It; ++It)
    {
        ARecastNavMesh* NavMesh = *It;
        if (NavMesh && !NavMesh->IsPendingKillPending())
        {
            if (GetAgentNameFromNavMesh(NavMesh) == AgentName)
            {
                return NavMesh;
            }
        }
    }

    UE_LOG(LogTemp, Warning,
        TEXT("NavMeshSerializer: NavMesh for agent [%s] not found"),
        *AgentName.ToString());
    return nullptr;
}

FString ANavMeshSerializer::GetNavDataFilePath(const FString& StageID, const FString& AgentName) const
{
    return FPaths::ProjectSavedDir() / TEXT("Stages") / (StageID + TEXT("_") + AgentName + TEXT(".navdata"));
}

bool ANavMeshSerializer::HasSavedNavMesh(const FString& StageID) const
{
    const FString SearchDir = FPaths::ProjectSavedDir() / TEXT("Stages");
    const FString Pattern = StageID + TEXT("_*.navdata");

    TArray<FString> FoundFiles;
    IFileManager::Get().FindFiles(FoundFiles, *(SearchDir / Pattern), true, false);

    return FoundFiles.Num() > 0;
}

void ANavMeshSerializer::DisableNavMeshAutoRebuild() const
{
    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (NavSys)
    {
        NavSys->SetNavigationOctreeLock(true);
    }
}

void ANavMeshSerializer::EnableNavMeshDynamicRebuild(FName AgentName)
{
    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!NavSys)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: NavigationSystem not found"));
        return;
    }

    ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
    if (!NavMesh)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Cannot enable rebuild — agent [%s] not found"),
            *AgentName.ToString());
        return;
    }

    NavSys->SetNavigationOctreeLock(false);
    NavMesh->ConditionalConstructGenerator();

    // 同時タイル生成ジョブ数を設定（0ならデフォルトのまま）
    if (MaxTileJobsCount > 0)
    {
        NavMesh->SetMaxSimultaneousTileGenerationJobsCount(MaxTileJobsCount);
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: Set MaxSimultaneousTileGenerationJobsCount=%d for agent [%s]"),
            MaxTileJobsCount, *AgentName.ToString());
    }

    NavMesh->RebuildAll();

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Dynamic rebuild triggered for agent [%s] (async tile-based)"),
        *AgentName.ToString());
}

void ANavMeshSerializer::EnableNavMeshDynamicRebuildAll()
{
    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!NavSys)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: NavigationSystem not found"));
        return;
    }

    NavSys->SetNavigationOctreeLock(false);

    TArray<ARecastNavMesh*> AllNavMeshes = GetAllRecastNavMeshes();
    for (ARecastNavMesh* NavMesh : AllNavMeshes)
    {
        NavMesh->ConditionalConstructGenerator();

        if (MaxTileJobsCount > 0)
        {
            NavMesh->SetMaxSimultaneousTileGenerationJobsCount(MaxTileJobsCount);
        }

        NavMesh->RebuildAll();
    }

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Dynamic rebuild triggered for all %d agent(s) (async tile-based)"),
        AllNavMeshes.Num());
}

//======================================================================//
// ビルド進捗モニタリング
//======================================================================//

void ANavMeshSerializer::StartBuildProgressMonitor()
{
    TArray<ARecastNavMesh*> AllNavMeshes = GetAllRecastNavMeshes();
    if (AllNavMeshes.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("NavMeshSerializer: No NavMesh to monitor"));
        return;
    }

    InitialBuildTaskCounts.Empty();
    AgentDebugMsgKeys.Empty();

    for (ARecastNavMesh* NavMesh : AllNavMeshes)
    {
        const FName AgentName = GetAgentNameFromNavMesh(NavMesh);
        AgentDebugMsgKeys.Add(AgentName, NextDebugMsgKey++);

        // 初期タスク数は0で仮登録。
        // RebuildAll直後はGeneratorがまだタスクをキューに積み終わっていない場合があるため、
        // Tick内のウォームアップフェーズで最大値を捕捉する。
        InitialBuildTaskCounts.Add(AgentName, 0);
    }

    bMonitoringBuildProgress = true;
    MonitorWarmupFrames = 0;
    SetActorTickEnabled(true);

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Build progress monitor started for %d agent(s)"),
        AllNavMeshes.Num());
}

void ANavMeshSerializer::StopBuildProgressMonitor()
{
    bMonitoringBuildProgress = false;
    SetActorTickEnabled(false);

    if (GEngine)
    {
        for (auto& Pair : AgentDebugMsgKeys)
        {
            GEngine->AddOnScreenDebugMessage(Pair.Value, 0.0f, FColor::Transparent, TEXT(""));
        }
    }

    UE_LOG(LogTemp, Log, TEXT("NavMeshSerializer: Build progress monitor stopped"));
}

void ANavMeshSerializer::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (!bMonitoringBuildProgress || !GEngine)
    {
        return;
    }

    // ウォームアップ: 最初の数十フレームはタスク数が増加していく可能性があるため
    // 各エージェントのGetNumRemaningBuildTasksの最大値を初期値として捕捉し続ける
    constexpr int32 WarmupDuration = 30; // 約0.5秒（60fps想定）
    const bool bInWarmup = (MonitorWarmupFrames < WarmupDuration);
    if (bInWarmup)
    {
        MonitorWarmupFrames++;
    }

    bool bAllComplete = true;

    for (auto& Pair : InitialBuildTaskCounts)
    {
        const FName AgentName = Pair.Key;
        int32& InitialTasks = Pair.Value;

        ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
        if (!NavMesh)
        {
            continue;
        }

        // GetGenerator() → FNavDataGenerator* → GetNumRemaningBuildTasks()
        const FNavDataGenerator* Generator = NavMesh->GetGenerator();
        const int32 RemainingTasks = Generator ? Generator->GetNumRemaningBuildTasks() : 0;

        // ウォームアップ中は観測された最大値で初期タスク数を更新
        if (bInWarmup && RemainingTasks > InitialTasks)
        {
            InitialTasks = RemainingTasks;
        }

        // 進捗率を計算
        double Progress = 100.0;
        if (InitialTasks > 0)
        {
            const int32 CompletedTasks = InitialTasks - RemainingTasks;
            Progress = FMath::Clamp(
                static_cast<double>(CompletedTasks) / static_cast<double>(InitialTasks) * 100.0,
                0.0,
                100.0
            );
        }

        const bool bIsBuilding = (RemainingTasks > 0);
        if (bIsBuilding)
        {
            bAllComplete = false;
        }

        // フォーマット: <エージェント名> : <パーセント(小数点第4位)>
        FString ProgressStr;
        if (bInWarmup && InitialTasks == 0)
        {
            // ウォームアップ中でまだタスクが0の場合 → 待機中表示
            ProgressStr = FString::Printf(
                TEXT("%s : Waiting..."),
                *AgentName.ToString()
            );
        }
        else
        {
            ProgressStr = FString::Printf(
                TEXT("%s : %.4f%%  (%d/%d tasks)"),
                *AgentName.ToString(),
                Progress,
                (InitialTasks - RemainingTasks),
                InitialTasks
            );
        }

        const int32* MsgKeyPtr = AgentDebugMsgKeys.Find(AgentName);
        if (MsgKeyPtr)
        {
            const FColor DisplayColor = bIsBuilding ? FColor::Yellow : FColor::Green;
            GEngine->AddOnScreenDebugMessage(
                *MsgKeyPtr,
                0.0f,
                DisplayColor,
                ProgressStr
            );
        }
    }

    // 全エージェントが完了（ウォームアップ完了後のみ判定）
    if (bAllComplete && !bInWarmup)
    {
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: All agents build complete. Stopping monitor."));

        for (auto& Pair : AgentDebugMsgKeys)
        {
            const FName AgentName = Pair.Key;
            const int32 InitialTasks = InitialBuildTaskCounts.FindRef(AgentName);
            const FString CompleteStr = FString::Printf(
                TEXT("%s : 100.0000%%  (%d/%d tasks) [Complete]"),
                *AgentName.ToString(),
                InitialTasks,
                InitialTasks
            );
            GEngine->AddOnScreenDebugMessage(
                Pair.Value,
                5.0f,
                FColor::Green,
                CompleteStr
            );
        }

        bMonitoringBuildProgress = false;
        SetActorTickEnabled(false);
    }
}

//======================================================================//
// 可視化
//======================================================================//

void ANavMeshSerializer::SetNavMeshVisualizationEnabled(bool bEnabled, FName AgentName)
{
    if (bEnabled)
    {
        ARecastNavMesh* NavMesh = FindNavMeshByAgentName(AgentName);
        if (!NavMesh)
        {
            UE_LOG(LogTemp, Error,
                TEXT("NavMeshSerializer: Cannot enable visualization — agent [%s] not found"),
                *AgentName.ToString());
            return;
        }

        EnabledVisAgents.Add(AgentName);
        BuildNavMeshVisualizationForAgent(NavMesh);
    }
    else
    {
        EnabledVisAgents.Remove(AgentName);
        ClearNavMeshVisualizationForAgent(AgentName);
    }
}

bool ANavMeshSerializer::IsNavMeshVisualizationEnabled(FName AgentName) const
{
    return EnabledVisAgents.Contains(AgentName);
}

void ANavMeshSerializer::BuildNavMeshVisualizationForAgent(ARecastNavMesh* NavMesh)
{
    if (!NavMesh) return;

    const FName AgentName = GetAgentNameFromNavMesh(NavMesh);

    TObjectPtr<UProceduralMeshComponent>* FoundComp = NavMeshVisComponents.Find(AgentName);
    UProceduralMeshComponent* VisMesh = nullptr;

    if (FoundComp && *FoundComp)
    {
        VisMesh = *FoundComp;
    }
    else
    {
        VisMesh = NewObject<UProceduralMeshComponent>(this);
        VisMesh->RegisterComponent();
        VisMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        VisMesh->bUseComplexAsSimpleCollision = false;
        NavMeshVisComponents.Add(AgentName, VisMesh);
    }

    VisMesh->ClearAllMeshSections();

    FRecastDebugGeometry DebugGeom;
    NavMesh->GetDebugGeometryForTile(DebugGeom, INDEX_NONE);

    if (DebugGeom.MeshVerts.IsEmpty())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("NavMeshSerializer: DebugGeom has no verts for agent [%s] (Actor=[%s])"),
            *AgentName.ToString(), *NavMesh->GetName());
        return;
    }

    TArray<FVector>  Vertices = DebugGeom.MeshVerts;
    TArray<int32>    Indices;
    TArray<FVector>  Normals;
    TArray<FVector2D> UV0;
    TArray<FColor>   Colors;
    TArray<FProcMeshTangent> Tangents;

    Normals.Init(FVector::UpVector, Vertices.Num());

    for (int32 AreaIdx = 0; AreaIdx < RECAST_MAX_AREAS; ++AreaIdx)
    {
        Indices.Append(DebugGeom.AreaIndices[AreaIdx]);
    }

    Colors.Init(FColor::White, Vertices.Num());
    UV0.Init(FVector2D::ZeroVector, Vertices.Num());

    VisMesh->CreateMeshSection(
        0, Vertices, Indices, Normals, UV0, Colors, Tangents, false
    );

    if (TObjectPtr<UMaterialInterface>* MatPtr = AgentVisMaterials.Find(AgentName))
    {
        if (*MatPtr)
        {
            VisMesh->SetMaterial(0, *MatPtr);
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning,
            TEXT("NavMeshSerializer: No material set for agent [%s] in AgentVisMaterials map."),
            *AgentName.ToString());
    }

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Visualization built for agent [%s] (Verts=%d Indices=%d)"),
        *AgentName.ToString(), Vertices.Num(), Indices.Num());
}

void ANavMeshSerializer::ClearNavMeshVisualizationForAgent(FName AgentName)
{
    if (TObjectPtr<UProceduralMeshComponent>* FoundComp = NavMeshVisComponents.Find(AgentName))
    {
        if (*FoundComp)
        {
            (*FoundComp)->ClearAllMeshSections();
        }
    }
}

//======================================================================//
// 保存
//======================================================================//

void ANavMeshSerializer::SaveNavMeshWhenReady(const FString& StageID)
{
    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!NavSys)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: NavigationSystem not found"));
        OnOperationFailed.Broadcast();
        return;
    }

    if (!NavSys->IsNavigationBuildInProgress())
    {
        SaveAllNavMeshes(StageID);
        return;
    }

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: NavMesh building in progress. "
            "Will save automatically when build completes."));

    PendingSaveStageID = StageID;
    NavSys->OnNavigationGenerationFinishedDelegate.AddDynamic(
        this, &ANavMeshSerializer::OnNavBuildFinished);
}

void ANavMeshSerializer::OnNavBuildFinished(ANavigationData* NavData)
{
    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (NavSys)
    {
        NavSys->OnNavigationGenerationFinishedDelegate.RemoveDynamic(
            this, &ANavMeshSerializer::OnNavBuildFinished);
    }

    SaveAllNavMeshes(PendingSaveStageID);
    PendingSaveStageID.Empty();
}

void ANavMeshSerializer::SaveAllNavMeshes(const FString& StageID)
{
    TArray<ARecastNavMesh*> AllNavMeshes = GetAllRecastNavMeshes();
    if (AllNavMeshes.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: No NavMesh to save"));
        OnOperationFailed.Broadcast();
        return;
    }

    const FString SaveDir = FPaths::ProjectSavedDir() / TEXT("Stages");
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.DirectoryExists(*SaveDir))
    {
        PlatformFile.CreateDirectoryTree(*SaveDir);
    }

    int32 SuccessCount = 0;
    for (ARecastNavMesh* NavMesh : AllNavMeshes)
    {
        const FName AgentName = GetAgentNameFromNavMesh(NavMesh);
        const FString FilePath = GetNavDataFilePath(StageID, AgentName.ToString());

        if (SaveSingleNavMesh(NavMesh, FilePath))
        {
            UE_LOG(LogTemp, Log,
                TEXT("NavMeshSerializer: Saved agent [%s] (Actor=[%s]) -> [%s]"),
                *AgentName.ToString(), *NavMesh->GetName(), *FilePath);
            ++SuccessCount;
        }
    }

    if (SuccessCount == AllNavMeshes.Num())
    {
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: All %d NavMesh(es) saved successfully for StageID [%s]"),
            SuccessCount, *StageID);
        OnSaveComplete.Broadcast();
    }
    else
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Only %d/%d NavMesh(es) saved for StageID [%s]"),
            SuccessCount, AllNavMeshes.Num(), *StageID);
        OnOperationFailed.Broadcast();
    }
}

bool ANavMeshSerializer::SaveSingleNavMesh(ARecastNavMesh* NavMesh, const FString& FilePath)
{
    if (!NavMesh) return false;

    TArray<uint8> NavMeshData;
    FMemoryWriter MemWriter(NavMeshData, true);
    FObjectAndNameAsStringProxyArchive Writer(MemWriter, false);
    NavMesh->Serialize(Writer);

    if (NavMeshData.Num() == 0)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Serialized data is empty for agent [%s]."),
            *GetAgentNameFromNavMesh(NavMesh).ToString());
        return false;
    }

    if (FFileHelper::SaveArrayToFile(NavMeshData, *FilePath))
    {
        return true;
    }

    UE_LOG(LogTemp, Error,
        TEXT("NavMeshSerializer: Failed to write file [%s]"), *FilePath);
    return false;
}

//======================================================================//
// ロード・適用
//======================================================================//

void ANavMeshSerializer::LoadAndApplyNavMesh(const FString& StageID, FVector StageOffset)
{
    LoadAndApplyAllNavMeshes(StageID, StageOffset);
}

void ANavMeshSerializer::LoadAndApplyAllNavMeshes(const FString& StageID, FVector StageOffset)
{
    TArray<ARecastNavMesh*> AllNavMeshes = GetAllRecastNavMeshes();
    if (AllNavMeshes.Num() == 0)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: No NavMesh actors in world to restore into"));
        OnOperationFailed.Broadcast();
        return;
    }

    DisableNavMeshAutoRebuild();

    int32 SuccessCount = 0;

    for (ARecastNavMesh* NavMesh : AllNavMeshes)
    {
        const FName AgentName = GetAgentNameFromNavMesh(NavMesh);
        const FString FilePath = GetNavDataFilePath(StageID, AgentName.ToString());

        if (FPaths::FileExists(FilePath))
        {
            if (LoadSingleNavMesh(NavMesh, FilePath, StageOffset))
            {
                UE_LOG(LogTemp, Log,
                    TEXT("NavMeshSerializer: Loaded agent [%s] (Actor=[%s]) <- [%s]"),
                    *AgentName.ToString(), *NavMesh->GetName(), *FilePath);
                ++SuccessCount;
            }
        }
        else
        {
            UE_LOG(LogTemp, Warning,
                TEXT("NavMeshSerializer: No saved data found for agent [%s] at [%s]"),
                *AgentName.ToString(), *FilePath);
        }
    }

    if (SuccessCount == 0)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: No NavMesh data loaded for StageID [%s]"), *StageID);
        OnOperationFailed.Broadcast();
        return;
    }

    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (NavSys)
    {
        PendingRegistrationCount = 0;
        for (ARecastNavMesh* NavMesh : AllNavMeshes)
        {
            NavSys->UnregisterNavData(NavMesh);
            NavSys->RequestRegistrationDeferred(*NavMesh);
            ++PendingRegistrationCount;
        }

        GetWorldTimerManager().SetTimer(
            RegistrationWaitHandle,
            this,
            &ANavMeshSerializer::OnNavDataRegistrationComplete,
            0.1f,
            false
        );
    }

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Loaded %d/%d NavMesh(es) for StageID [%s]"),
        SuccessCount, AllNavMeshes.Num(), *StageID);

    OnLoadComplete.Broadcast();
}

bool ANavMeshSerializer::LoadSingleNavMesh(ARecastNavMesh* NavMesh, const FString& FilePath, FVector StageOffset)
{
    if (!NavMesh) return false;

    TArray<uint8> NavMeshData;
    if (!FFileHelper::LoadFileToArray(NavMeshData, *FilePath))
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Failed to read file [%s]"), *FilePath);
        return false;
    }

    FMemoryReader MemReader(NavMeshData, true);
    FObjectAndNameAsStringProxyArchive Reader(MemReader, true);
    NavMesh->Serialize(Reader);

    if (!StageOffset.IsNearlyZero())
    {
        NavMesh->ApplyWorldOffset(StageOffset, false);
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: Applied offset %s to agent [%s]"),
            *StageOffset.ToString(), *GetAgentNameFromNavMesh(NavMesh).ToString());
    }

    return true;
}

void ANavMeshSerializer::OnNavDataRegistrationComplete()
{
    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: NavData registration complete for all agents."));
    OnLoadComplete.Broadcast();
}