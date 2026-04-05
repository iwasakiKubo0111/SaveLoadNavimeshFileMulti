#include "NavMeshSerializer.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Detour/DetourNavMesh.h"

// ──────────────────────────────────────────────────────────────────────
// ファイルフォーマット定義
// ──────────────────────────────────────────────────────────────────────
namespace NavTileFile
{
    static constexpr uint32 Magic = 0x4E415654; // "NAVT"
    static constexpr uint32 Version = 2;

    struct FHeader
    {
        uint32 Magic;
        uint32 Version;
        int32  TileCount;
        dtNavMeshParams NavMeshParams;
    };
}

// ──────────────────────────────────────────────────────────────────────
// 初期化
// ──────────────────────────────────────────────────────────────────────

ANavMeshSerializer::ANavMeshSerializer()
{
    PrimaryActorTick.bCanEverTick = false;
}

void ANavMeshSerializer::BeginPlay()
{
    Super::BeginPlay();

    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (NavSys)
    {
        NavSys->SetNavigationOctreeLock(true);
        NavSys->CancelBuild();
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: Auto rebuild disabled on BeginPlay."));
    }
}

// ──────────────────────────────────────────────────────────────────────
// ユーティリティ
// ──────────────────────────────────────────────────────────────────────

FString ANavMeshSerializer::MakeTileKey(const FString& StageID, const FName& AgentName)
{
    if (AgentName == NAME_None) return StageID;
    return StageID + TEXT("_") + AgentName.ToString();
}

FString ANavMeshSerializer::GetNavDataFilePath(
    const FString& StageID, const FName& AgentName) const
{
    if (AgentName == NAME_None)
    {
        return FPaths::ProjectSavedDir() / TEXT("Stages") / StageID + TEXT(".navtiles");
    }
    return FPaths::ProjectSavedDir() / TEXT("Stages")
        / StageID + TEXT("_") + AgentName.ToString() + TEXT(".navtiles");
}

bool ANavMeshSerializer::HasSavedNavMesh(
    const FString& StageID, const FName& AgentName) const
{
    return FPaths::FileExists(GetNavDataFilePath(StageID, AgentName));
}

TArray<FString> ANavMeshSerializer::GetLoadedStageIDs() const
{
    TArray<FString> Out;
    LoadedTileRefs.GetKeys(Out);
    return Out;
}

ARecastNavMesh* ANavMeshSerializer::GetRecastNavMeshForAgent(
    const FName& AgentName) const
{
    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!NavSys)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: NavigationSystem not found"));
        return nullptr;
    }

    if (AgentName == NAME_None)
    {
        ARecastNavMesh* NavMesh =
            Cast<ARecastNavMesh>(NavSys->GetDefaultNavDataInstance());
        if (!NavMesh)
        {
            UE_LOG(LogTemp, Error,
                TEXT("NavMeshSerializer: Default RecastNavMesh not found"));
        }
        return NavMesh;
    }

    for (ANavigationData* NavData : NavSys->NavDataSet)
    {
        ARecastNavMesh* NavMesh = Cast<ARecastNavMesh>(NavData);
        if (!NavMesh) continue;

        const FNavDataConfig& Config = NavMesh->GetConfig();
        if (Config.Name == AgentName)
        {
            return NavMesh;
        }
    }

    UE_LOG(LogTemp, Error,
        TEXT("NavMeshSerializer: RecastNavMesh not found for Agent [%s]"),
        *AgentName.ToString());
    return nullptr;
}

// ──────────────────────────────────────────────────────────────────────
// 可視化
// ──────────────────────────────────────────────────────────────────────

void ANavMeshSerializer::BuildNavMeshVisualization(const FName& AgentName)
{
    ARecastNavMesh* NavMesh = GetRecastNavMeshForAgent(AgentName);
    if (!NavMesh) return;

    if (!NavMeshVisMesh)
    {
        NavMeshVisMesh = NewObject<UProceduralMeshComponent>(this);
        NavMeshVisMesh->RegisterComponent();
        NavMeshVisMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        NavMeshVisMesh->bUseComplexAsSimpleCollision = false;
    }

    // このエージェント用のSectionIndexを取得 or 新規割り当て
    int32& SectionIndex = AgentVisSectionMap.FindOrAdd(AgentName, NextVisSectionIndex);
    if (SectionIndex == NextVisSectionIndex)
    {
        ++NextVisSectionIndex;
    }

    FRecastDebugGeometry DebugGeom;
    NavMesh->GetDebugGeometry(DebugGeom);

    if (DebugGeom.MeshVerts.IsEmpty())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("NavMeshSerializer: DebugGeom has no verts for Agent=[%s]. "
                "NavMesh may not be built."),
            *AgentName.ToString());
        return;
    }

    TArray<FVector>          Vertices;
    TArray<int32>            Indices;
    TArray<FVector>          Normals;
    TArray<FVector2D>        UV0;
    TArray<FColor>           Colors;
    TArray<FProcMeshTangent> Tangents;

    Vertices = DebugGeom.MeshVerts;
    Normals.Init(FVector::UpVector, Vertices.Num());

    for (int32 AreaIdx = 0; AreaIdx < RECAST_MAX_AREAS; ++AreaIdx)
    {
        Indices.Append(DebugGeom.AreaIndices[AreaIdx]);
    }

    // AgentNameのハッシュから色を生成 (エージェントごとに異なる色)
    FColor VisColor = FColor(0, 255, 100, 120);
    if (AgentName != NAME_None)
    {
        const uint32 Hash = GetTypeHash(AgentName);
        VisColor = FColor(
            (Hash & 0xFF),
            ((Hash >> 8) & 0xFF),
            ((Hash >> 16) & 0xFF),
            120
        );
    }

    Colors.Init(VisColor, Vertices.Num());
    UV0.Init(FVector2D::ZeroVector, Vertices.Num());

    NavMeshVisMesh->CreateMeshSection(
        SectionIndex, Vertices, Indices, Normals, UV0, Colors, Tangents, false);

    // マテリアル適用: AgentName で引いて、なければ NAME_None のデフォルトを使う
    UMaterialInterface* Mat = nullptr;
    if (const TObjectPtr<UMaterialInterface>* Found = NavVisMaterialMap.Find(AgentName))
    {
        Mat = *Found;
    }
    else if (const TObjectPtr<UMaterialInterface>* Default = NavVisMaterialMap.Find(NAME_None))
    {
        Mat = *Default;
    }

    if (Mat)
    {
        NavMeshVisMesh->SetMaterial(SectionIndex, Mat);
    }

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Visualization built for Agent=[%s] Section=%d. "
            "Verts=%d Indices=%d"),
        *AgentName.ToString(), SectionIndex, Vertices.Num(), Indices.Num());
}

void ANavMeshSerializer::ClearNavMeshVisualization(const FName& AgentName)
{
    if (!NavMeshVisMesh) return;

    if (AgentName == NAME_None)
    {
        // 全エージェントの可視化をクリア
        NavMeshVisMesh->ClearAllMeshSections();
        AgentVisSectionMap.Empty();
        NextVisSectionIndex = 0;
    }
    else
    {
        // 対象エージェントのセクションのみクリア
        const int32* SectionIndex = AgentVisSectionMap.Find(AgentName);
        if (SectionIndex)
        {
            NavMeshVisMesh->ClearMeshSection(*SectionIndex);
            AgentVisSectionMap.Remove(AgentName);
        }
    }
}

void ANavMeshSerializer::SetNavMeshVisualizationEnabled(
    bool bEnabled, const FName& AgentName)
{
    bIsVisualizationEnabled = bEnabled;

    if (bEnabled)
    {
        BuildNavMeshVisualization(AgentName);
    }
    else
    {
        ClearNavMeshVisualization(AgentName);
    }
}

void ANavMeshSerializer::SetViewMode(EViewModeIndex ViewMode)
{
    if (!GEngine || !GEngine->GameViewport) return;

    UGameViewportClient* Viewport = GEngine->GameViewport;

    if (ViewMode == VMI_Unlit)
    {
        if (!SavedShowFlags.IsSet())
        {
            SavedShowFlags = Viewport->EngineShowFlags;
            SavedViewModeIndex = static_cast<EViewModeIndex>(Viewport->ViewModeIndex);
        }
        ApplyViewMode(VMI_Unlit, false, Viewport->EngineShowFlags);
        Viewport->ViewModeIndex = VMI_Unlit;
    }
    else
    {
        if (SavedShowFlags.IsSet())
        {
            Viewport->EngineShowFlags = SavedShowFlags.GetValue();
            Viewport->ViewModeIndex = SavedViewModeIndex.GetValue();
            SavedShowFlags.Reset();
            SavedViewModeIndex.Reset();
        }
    }
}

// ──────────────────────────────────────────────────────────────────────
// 保存
// ──────────────────────────────────────────────────────────────────────

void ANavMeshSerializer::SaveNavTilesWhenReady(
    const FString& StageID, const FName& AgentName)
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
        SaveNavTiles(StageID, AgentName);
        return;
    }

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Build in progress. Will save when complete. "
            "Stage=[%s] Agent=[%s]"),
        *StageID, *AgentName.ToString());

    PendingSaveStageID = StageID;
    PendingSaveAgentName = AgentName;
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

    SaveNavTiles(PendingSaveStageID, PendingSaveAgentName);
    PendingSaveStageID.Empty();
    PendingSaveAgentName = NAME_None;
}

void ANavMeshSerializer::EnableNavMeshDynamicRebuild()
{
    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!NavSys)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: NavigationSystem not found"));
        return;
    }

    NavSys->SetNavigationOctreeLock(false);

    for (ANavigationData* NavData : NavSys->NavDataSet)
    {
        ARecastNavMesh* NavMesh = Cast<ARecastNavMesh>(NavData);
        if (NavMesh)
        {
            NavMesh->ConditionalConstructGenerator();
        }
    }

    NavSys->Build();
    UE_LOG(LogTemp, Log, TEXT("NavMeshSerializer: Dynamic rebuild enabled."));
}

void ANavMeshSerializer::SaveNavTiles(
    const FString& StageID, const FName& AgentName)
{
    ARecastNavMesh* NavMesh = GetRecastNavMeshForAgent(AgentName);
    if (!NavMesh) { OnOperationFailed.Broadcast(); return; }

    const dtNavMesh* DetourMesh = NavMesh->GetRecastMesh();
    if (!DetourMesh)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: dtNavMesh is null"));
        OnOperationFailed.Broadcast();
        return;
    }

    struct FTileData { TArray<uint8> Bytes; int32 TileX; int32 TileY; int32 Layer; };
    TArray<FTileData> CollectedTiles;

    const int32 MaxTiles = DetourMesh->getMaxTiles();
    for (int32 i = 0; i < MaxTiles; ++i)
    {
        const dtMeshTile* Tile = DetourMesh->getTile(i);
        if (!Tile || !Tile->header || Tile->dataSize == 0) continue;

        FTileData TData;
        TData.Bytes.SetNumUninitialized(Tile->dataSize);
        FMemory::Memcpy(TData.Bytes.GetData(), Tile->data, Tile->dataSize);
        TData.TileX = Tile->header->x;
        TData.TileY = Tile->header->y;
        TData.Layer = Tile->header->layer;
        CollectedTiles.Add(MoveTemp(TData));
    }

    if (CollectedTiles.Num() == 0)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: No tiles found for Stage=[%s] Agent=[%s]"),
            *StageID, *AgentName.ToString());
        OnOperationFailed.Broadcast();
        return;
    }

    const dtNavMeshParams* RawParams = DetourMesh->getParams();
    dtNavMeshParams SafeParams = *RawParams;
    if (SafeParams.maxPolys <= 0 || SafeParams.maxPolys > 0x8000)
    {
        SafeParams.maxPolys = 2048;
    }

    TArray<uint8> FileData;
    FMemoryWriter Writer(FileData);

    NavTileFile::FHeader Header;
    Header.Magic = NavTileFile::Magic;
    Header.Version = NavTileFile::Version;
    Header.TileCount = CollectedTiles.Num();
    Header.NavMeshParams = SafeParams;
    Writer.Serialize(&Header, sizeof(Header));

    for (FTileData& TD : CollectedTiles)
    {
        int32 Size = TD.Bytes.Num();
        Writer.Serialize(&Size, sizeof(int32));
        Writer.Serialize(TD.Bytes.GetData(), Size);
    }

    const FString FilePath = GetNavDataFilePath(StageID, AgentName);
    const FString SaveDir = FPaths::GetPath(FilePath);
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    if (!PF.DirectoryExists(*SaveDir)) PF.CreateDirectoryTree(*SaveDir);

    if (FFileHelper::SaveArrayToFile(FileData, *FilePath))
    {
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: Saved Stage=[%s] Agent=[%s] "
                "(%d tiles, %d bytes, maxTiles=%d maxPolys=%d)"),
            *StageID, *AgentName.ToString(),
            CollectedTiles.Num(), FileData.Num(),
            SafeParams.maxTiles, SafeParams.maxPolys);
        OnSaveComplete.Broadcast();
    }
    else
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Failed to write [%s]"), *FilePath);
        OnOperationFailed.Broadcast();
    }
}

// ──────────────────────────────────────────────────────────────────────
// ロード・マージ
// ──────────────────────────────────────────────────────────────────────

void ANavMeshSerializer::LoadAndMergeNavTiles(
    const FString& StageID, const FName& AgentName, FVector StageOffset)
{
    // ── 計測開始 ──
    const double TimeStart = FPlatformTime::Seconds();
    double TimeAfterFileLoad = 0.0;
    double TimeAfterTileAdd = 0.0;
    double TimeAfterReconnect = 0.0;
    double TimeAfterVisualization = 0.0;

    const FString TileKey = MakeTileKey(StageID, AgentName);

    if (LoadedTileRefs.Contains(TileKey))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("NavMeshSerializer: [%s] is already loaded. Unload first."), *TileKey);
        OnLoadComplete.Broadcast();
        return;
    }

    const FString FilePath = GetNavDataFilePath(StageID, AgentName);
    if (!FPaths::FileExists(FilePath))
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: File not found [%s]"), *FilePath);
        OnOperationFailed.Broadcast();
        return;
    }

    TArray<uint8> FileData;
    if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Failed to read [%s]"), *FilePath);
        OnOperationFailed.Broadcast();
        return;
    }
    TimeAfterFileLoad = FPlatformTime::Seconds();

    ARecastNavMesh* NavMesh = GetRecastNavMeshForAgent(AgentName);
    if (!NavMesh) { OnOperationFailed.Broadcast(); return; }

    dtNavMesh* DetourMesh = NavMesh->GetRecastMesh();
    if (!DetourMesh)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: dtNavMesh is null"));
        OnOperationFailed.Broadcast();
        return;
    }

    // ── ファイル読み込み・検証 ──
    FMemoryReader Reader(FileData);

    NavTileFile::FHeader Header;
    Reader.Serialize(&Header, sizeof(Header));

    if (Header.Magic != NavTileFile::Magic || Header.Version != NavTileFile::Version)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: Invalid file format [%s]"), *FilePath);
        OnOperationFailed.Broadcast();
        return;
    }

    // ── Params不一致チェック → 再初期化 ──
    const dtNavMeshParams* CurrentParams = DetourMesh->getParams();
    if (CurrentParams->maxTiles != Header.NavMeshParams.maxTiles ||
        CurrentParams->maxPolys != Header.NavMeshParams.maxPolys)
    {
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: Params mismatch. Reinitializing. "
                "current(maxTiles=%d maxPolys=%d) file(maxTiles=%d maxPolys=%d)"),
            CurrentParams->maxTiles, CurrentParams->maxPolys,
            Header.NavMeshParams.maxTiles, Header.NavMeshParams.maxPolys);

        TArray<FString> KeysToRemove;
        for (auto& Pair : LoadedTileRefs)
        {
            if (AgentName == NAME_None ||
                Pair.Key.EndsWith(TEXT("_") + AgentName.ToString()) ||
                Pair.Key == AgentName.ToString())
            {
                for (uint64 Ref : Pair.Value)
                    DetourMesh->removeTile(static_cast<dtTileRef>(Ref), nullptr, nullptr);
                KeysToRemove.Add(Pair.Key);
            }
        }
        for (const FString& Key : KeysToRemove)
            LoadedTileRefs.Remove(Key);

        dtStatus InitStatus = DetourMesh->init(&Header.NavMeshParams);
        if (dtStatusFailed(InitStatus))
        {
            UE_LOG(LogTemp, Error,
                TEXT("NavMeshSerializer: dtNavMesh re-init failed (Status=0x%X)"),
                InitStatus);
            OnOperationFailed.Broadcast();
            return;
        }

        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: dtNavMesh re-initialized (maxTiles=%d maxPolys=%d)"),
            Header.NavMeshParams.maxTiles, Header.NavMeshParams.maxPolys);
    }

    // ── オフセット変換: UE座標 → Recast座標 ──
    const bool bHasOffset = !StageOffset.IsNearlyZero();
    const dtReal OffX = (dtReal)StageOffset.X;
    const dtReal OffY = (dtReal)StageOffset.Z;
    const dtReal OffZ = (dtReal)-StageOffset.Y;

    int32 TileOffsetX = 0;
    int32 TileOffsetY = 0;
    if (bHasOffset)
    {
        const dtNavMeshParams* Params = DetourMesh->getParams();
        TileOffsetX = FMath::RoundToInt(OffX / Params->tileWidth);
        TileOffsetY = FMath::RoundToInt(OffZ / Params->tileHeight);

        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: StageOffset UE=(%.1f,%.1f,%.1f) "
                "Recast=(%.1f,%.1f,%.1f) TileOffset=(%d,%d) tileSize=(%.1f,%.1f)"),
            StageOffset.X, StageOffset.Y, StageOffset.Z,
            OffX, OffY, OffZ,
            TileOffsetX, TileOffsetY,
            Params->tileWidth, Params->tileHeight);
    }

    TArray<uint64> AddedRefs;

    for (int32 TileIdx = 0; TileIdx < Header.TileCount; ++TileIdx)
    {
        int32 DataSize = 0;
        Reader.Serialize(&DataSize, sizeof(int32));
        if (DataSize <= 0) continue;

        unsigned char* TileData =
            static_cast<unsigned char*>(dtAlloc(DataSize, DT_ALLOC_TEMP));
        if (!TileData)
        {
            UE_LOG(LogTemp, Error,
                TEXT("NavMeshSerializer: dtAlloc failed for tile %d"), TileIdx);
            continue;
        }
        Reader.Serialize(TileData, DataSize);

        if (bHasOffset)
        {
            dtMeshHeader* TileHeader = reinterpret_cast<dtMeshHeader*>(TileData);
            TileHeader->bmin[0] += OffX; TileHeader->bmin[1] += OffY; TileHeader->bmin[2] += OffZ;
            TileHeader->bmax[0] += OffX; TileHeader->bmax[1] += OffY; TileHeader->bmax[2] += OffZ;
            TileHeader->x += TileOffsetX;
            TileHeader->y += TileOffsetY;
        }

        dtTileRef TileRef = 0;
        dtStatus Status = DetourMesh->addTile(
            TileData, DataSize, DT_TILE_FREE_DATA, 0, &TileRef);

        if (dtStatusFailed(Status))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("NavMeshSerializer: addTile failed tile=%d [%s] Status=0x%X"),
                TileIdx, *TileKey, Status);
            dtFree(TileData, DT_ALLOC_TEMP);
            continue;
        }

        if (bHasOffset)
        {
            const dtMeshTile* AddedTile = DetourMesh->getTileByRef(TileRef);
            if (AddedTile && AddedTile->verts && AddedTile->header)
            {
                const int32 VertCount = AddedTile->header->vertCount;
                dtReal* Verts = const_cast<dtReal*>(AddedTile->verts);
                for (int32 v = 0; v < VertCount; ++v)
                {
                    Verts[v * 3 + 0] += OffX;
                    Verts[v * 3 + 1] += OffY;
                    Verts[v * 3 + 2] += OffZ;
                }

                if (AddedTile->detailVerts && AddedTile->header->detailVertCount > 0)
                {
                    const int32 DVertCount = AddedTile->header->detailVertCount;
                    dtReal* DVerts = const_cast<dtReal*>(AddedTile->detailVerts);
                    for (int32 v = 0; v < DVertCount; ++v)
                    {
                        DVerts[v * 3 + 0] += OffX;
                        DVerts[v * 3 + 1] += OffY;
                        DVerts[v * 3 + 2] += OffZ;
                    }
                }

                if (AddedTile->offMeshCons && AddedTile->header->offMeshConCount > 0)
                {
                    const int32 ConCount = AddedTile->header->offMeshConCount;
                    for (int32 c = 0; c < ConCount; ++c)
                    {
                        dtOffMeshConnection& Con =
                            const_cast<dtOffMeshConnection&>(AddedTile->offMeshCons[c]);
                        Con.pos[0] += OffX; Con.pos[1] += OffY; Con.pos[2] += OffZ;
                        Con.pos[3] += OffX; Con.pos[4] += OffY; Con.pos[5] += OffZ;
                    }
                }
            }
        }

        AddedRefs.Add(static_cast<uint64>(TileRef));
    }
    TimeAfterTileAdd = FPlatformTime::Seconds();

    // ── 境界リンク再構築 ──
    if (AddedRefs.Num() > 0)
    {
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: Reconnecting tile boundary links for [%s]..."),
            *TileKey);

        TArray<uint64> ReaddedRefs;
        for (uint64 OldRef : AddedRefs)
        {
            unsigned char* TileData = nullptr;
            int32          TileDataSize = 0;
            dtStatus RemoveStatus = DetourMesh->removeTile(
                static_cast<dtTileRef>(OldRef), &TileData, &TileDataSize);

            if (dtStatusFailed(RemoveStatus) || !TileData || TileDataSize == 0)
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("NavMeshSerializer: removeTile failed during reconnect (Ref=%llu)"),
                    OldRef);
                continue;
            }

            dtTileRef NewRef = 0;
            dtStatus AddStatus = DetourMesh->addTile(
                TileData, TileDataSize, DT_TILE_FREE_DATA, 0, &NewRef);

            if (dtStatusFailed(AddStatus))
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("NavMeshSerializer: re-addTile failed during reconnect (Status=0x%X)"),
                    AddStatus);
                dtFree(TileData, DT_ALLOC_TEMP);
                continue;
            }

            ReaddedRefs.Add(static_cast<uint64>(NewRef));
        }
        AddedRefs = MoveTemp(ReaddedRefs);

        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: Boundary links reconnected for [%s] (%d tiles)"),
            *TileKey, AddedRefs.Num());
    }
    TimeAfterReconnect = FPlatformTime::Seconds();

    if (AddedRefs.Num() == 0)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: No tiles added for [%s]"), *TileKey);
        OnOperationFailed.Broadcast();
        return;
    }

    LoadedTileRefs.Add(TileKey, MoveTemp(AddedRefs));

    // ── UE NavigationSystem への通知と自動リビルド抑制 ──
    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (NavSys)
    {
        NavSys->SetNavigationOctreeLock(true);
        NavSys->CancelBuild();
    }

    NavMesh->ConditionalConstructGenerator();
    NavMesh->RequestDrawingUpdate();

    // 可視化を更新
    SetNavMeshVisualizationEnabled(false, AgentName);
    SetNavMeshVisualizationEnabled(true, AgentName);

    TimeAfterVisualization = FPlatformTime::Seconds();

    // ── 計測結果ログ ──
    const double TotalMs = (TimeAfterVisualization - TimeStart) * 1000.0;
    const double FileLoadMs = (TimeAfterFileLoad - TimeStart) * 1000.0;
    const double TileAddMs = (TimeAfterTileAdd - TimeAfterFileLoad) * 1000.0;
    const double ReconnectMs = (TimeAfterReconnect - TimeAfterTileAdd) * 1000.0;
    const double VisualizationMs = (TimeAfterVisualization - TimeAfterReconnect) * 1000.0;

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: [LoadAndMergeNavTiles] Stage=[%s] Agent=[%s]\n")
        TEXT("  Total:         %.2f ms\n")
        TEXT("  FileLoad:      %.2f ms\n")
        TEXT("  TileAdd:       %.2f ms  (%d tiles)\n")
        TEXT("  Reconnect:     %.2f ms\n")
        TEXT("  Visualization: %.2f ms"),
        *StageID, *AgentName.ToString(),
        TotalMs,
        FileLoadMs,
        TileAddMs, Header.TileCount,
        ReconnectMs,
        VisualizationMs
    );

    OnLoadComplete.Broadcast();
}

// ──────────────────────────────────────────────────────────────────────
// アンロード
// ──────────────────────────────────────────────────────────────────────

void ANavMeshSerializer::UnloadNavTiles(
    const FString& StageID, const FName& AgentName)
{
    // ── 計測開始 ──
    const double TimeStart = FPlatformTime::Seconds();
    double TimeAfterRemoveTile = 0.0;
    double TimeAfterVisualization = 0.0;

    const FString TileKey = MakeTileKey(StageID, AgentName);

    const TArray<uint64>* Refs = LoadedTileRefs.Find(TileKey);
    if (!Refs || Refs->Num() == 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("NavMeshSerializer: [%s] is not loaded."), *TileKey);
        return;
    }

    ARecastNavMesh* NavMesh = GetRecastNavMeshForAgent(AgentName);
    if (!NavMesh) return;

    dtNavMesh* DetourMesh = NavMesh->GetRecastMesh();
    if (!DetourMesh) return;

    int32 RemovedCount = 0;
    for (uint64 Ref : *Refs)
    {
        dtStatus Status = DetourMesh->removeTile(
            static_cast<dtTileRef>(Ref), nullptr, nullptr);
        if (dtStatusSucceed(Status))
        {
            ++RemovedCount;
        }
        else
        {
            UE_LOG(LogTemp, Warning,
                TEXT("NavMeshSerializer: removeTile failed (Ref=%llu)"), Ref);
        }
    }

    LoadedTileRefs.Remove(TileKey);
    TimeAfterRemoveTile = FPlatformTime::Seconds();

    NavMesh->ConditionalConstructGenerator();
    NavMesh->RequestDrawingUpdate();

    // 可視化を更新
    SetNavMeshVisualizationEnabled(false, AgentName);
    SetNavMeshVisualizationEnabled(true, AgentName);

    TimeAfterVisualization = FPlatformTime::Seconds();

    // 全地形がアンロードされた場合のみ自動リビルドを再開
    if (LoadedTileRefs.Num() == 0)
    {
        UNavigationSystemV1* NavSys =
            FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
        if (NavSys)
        {
            NavSys->SetNavigationOctreeLock(false);
            UE_LOG(LogTemp, Log,
                TEXT("NavMeshSerializer: All stages unloaded. Auto rebuild re-enabled."));
        }
    }
    else
    {
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: [%s] unloaded. %d stage(s) still loaded."),
            *TileKey, LoadedTileRefs.Num());
    }

    // ── 計測結果ログ ──
    const double TotalMs = (TimeAfterVisualization - TimeStart) * 1000.0;
    const double RemoveTileMs = (TimeAfterRemoveTile - TimeStart) * 1000.0;
    const double VisualizationMs = (TimeAfterVisualization - TimeAfterRemoveTile) * 1000.0;

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: [UnloadNavTiles] Stage=[%s] Agent=[%s]\n")
        TEXT("  Total:         %.2f ms\n")
        TEXT("  RemoveTile:    %.2f ms  (%d tiles)\n")
        TEXT("  Visualization: %.2f ms"),
        *StageID, *AgentName.ToString(),
        TotalMs,
        RemoveTileMs, RemovedCount,
        VisualizationMs
    );

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Unloaded Stage=[%s] Agent=[%s] (%d tiles removed)"),
        *StageID, *AgentName.ToString(), RemovedCount);
}