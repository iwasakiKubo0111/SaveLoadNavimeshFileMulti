#include "NavMeshSerializer.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

// ──────────────────────────────────────────────────────────────────────
// 初期化
// ──────────────────────────────────────────────────────────────────────

ANavMeshSerializer::ANavMeshSerializer()
{
    PrimaryActorTick.bCanEverTick = true; // Tick有効化
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
// Tick: フレーム分割ロード処理
// ──────────────────────────────────────────────────────────────────────

void ANavMeshSerializer::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (PendingLoads.Num() > 0)
    {
        TickPendingLoads();
    }
    else if (PendingUnloads.Num() > 0)
    {
        TickPendingUnloads();
    }
}

void ANavMeshSerializer::TickPendingLoads()
{
    // 先頭の PendingLoad を処理 (複数同時ロード時はキュー順に1つずつ)
    FPendingLoadState& State = PendingLoads[0];

    ARecastNavMesh* NavMesh = GetRecastNavMeshForAgent(State.AgentName);
    if (!NavMesh)
    {
        PendingLoads.RemoveAt(0);
        OnOperationFailed.Broadcast();
        return;
    }

    dtNavMesh* DetourMesh = NavMesh->GetRecastMesh();
    if (!DetourMesh)
    {
        PendingLoads.RemoveAt(0);
        OnOperationFailed.Broadcast();
        return;
    }

    // フェーズに応じた処理を実行
    switch (State.Phase)
    {
    case FPendingLoadState::EPhase::TileAdd:
        TickTileAdd(State, DetourMesh);
        break;

    case FPendingLoadState::EPhase::Relink:
        TickRelink(State, DetourMesh, NavMesh);
        break;
    }
}

void ANavMeshSerializer::TickPendingUnloads()
{
    FPendingUnloadState& State = PendingUnloads[0];

    ARecastNavMesh* NavMesh = GetRecastNavMeshForAgent(State.AgentName);
    if (!NavMesh)
    {
        PendingUnloads.RemoveAt(0);
        OnOperationFailed.Broadcast();
        return;
    }

    dtNavMesh* DetourMesh = NavMesh->GetRecastMesh();
    if (!DetourMesh)
    {
        PendingUnloads.RemoveAt(0);
        OnOperationFailed.Broadcast();
        return;
    }

    // TilesPerFrame 枚ずつ removeTile
    int32 ProcessedThisFrame = 0;
    while (State.NextIndex < State.Refs.Num()
        && ProcessedThisFrame < TilesPerFrame)
    {
        dtStatus Status = DetourMesh->removeTile(
            static_cast<dtTileRef>(State.Refs[State.NextIndex]),
            nullptr, nullptr);

        if (dtStatusFailed(Status))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("NavMeshSerializer: removeTile failed (Ref=%llu)"),
                State.Refs[State.NextIndex]);
        }

        State.NextIndex++;
        ProcessedThisFrame++;
    }

    // 全タイル remove 完了
    if (State.NextIndex >= State.Refs.Num())
    {
        // ── 後処理（FinalizePendingLoad と対称） ──
        NavMesh->ConditionalConstructGenerator();

        if (bIsVisualizationEnabled)
        {
            SetNavMeshVisualizationEnabled(false, State.AgentName);
            SetNavMeshVisualizationEnabled(true, State.AgentName);
        }

        // 全地形アンロード完了時のみ自動リビルド再開
        if (LoadedTileRefs.Num() == 0 && PendingUnloads.Num() <= 1)
        {
            UNavigationSystemV1* NavSys =
                FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
            if (NavSys)
            {
                NavSys->SetNavigationOctreeLock(false);
            }
        }

        PendingUnloads.RemoveAt(0);
    }
}

// ──────────────────────────────────────────────────────────────────────
// TileAdd フェーズ
// 1フレームあたり TilesPerFrame 枚だけ addTile を実行する
// ──────────────────────────────────────────────────────────────────────

void ANavMeshSerializer::TickTileAdd(FPendingLoadState& State, dtNavMesh* DetourMesh)
{
    // Reader の位置を前回の続きから復元
    FMemoryReader Reader(State.FileData);
    Reader.Seek(State.ReaderOffset);

    int32 ProcessedThisFrame = 0;
    while (State.NextTileIndex < State.Header.TileCount
        && ProcessedThisFrame < TilesPerFrame)
    {
        int32 DataSize = 0;
        Reader.Serialize(&DataSize, sizeof(int32));
        if (DataSize <= 0)
        {
            State.NextTileIndex++;
            continue;
        }

        unsigned char* TileData =
            static_cast<unsigned char*>(dtAlloc(DataSize, DT_ALLOC_TEMP));
        if (!TileData)
        {
            State.NextTileIndex++;
            continue;
        }
        Reader.Serialize(TileData, DataSize);

        // オフセットがある場合はヘッダの bmin/bmax・タイル座標を補正
        if (State.bHasOffset)
        {
            dtMeshHeader* TileHeader = reinterpret_cast<dtMeshHeader*>(TileData);
            TileHeader->bmin[0] += State.OffX; TileHeader->bmin[1] += State.OffY; TileHeader->bmin[2] += State.OffZ;
            TileHeader->bmax[0] += State.OffX; TileHeader->bmax[1] += State.OffY; TileHeader->bmax[2] += State.OffZ;
            TileHeader->x += State.TileOffsetX;
            TileHeader->y += State.TileOffsetY;
        }

        dtTileRef TileRef = 0;
        dtStatus Status = DetourMesh->addTile(
            TileData, DataSize, DT_TILE_FREE_DATA, 0, &TileRef);

        if (dtStatusFailed(Status))
        {
            dtFree(TileData, DT_ALLOC_TEMP);
        }
        else
        {
            // オフセットがある場合は追加後のタイルの頂点座標も補正
            if (State.bHasOffset)
            {
                const dtMeshTile* AddedTile = DetourMesh->getTileByRef(TileRef);
                if (AddedTile && AddedTile->verts && AddedTile->header)
                {
                    const int32 VertCount = AddedTile->header->vertCount;
                    dtReal* Verts = const_cast<dtReal*>(AddedTile->verts);
                    for (int32 v = 0; v < VertCount; ++v)
                    {
                        Verts[v * 3 + 0] += State.OffX;
                        Verts[v * 3 + 1] += State.OffY;
                        Verts[v * 3 + 2] += State.OffZ;
                    }

                    if (AddedTile->detailVerts && AddedTile->header->detailVertCount > 0)
                    {
                        const int32 DVertCount = AddedTile->header->detailVertCount;
                        dtReal* DVerts = const_cast<dtReal*>(AddedTile->detailVerts);
                        for (int32 v = 0; v < DVertCount; ++v)
                        {
                            DVerts[v * 3 + 0] += State.OffX;
                            DVerts[v * 3 + 1] += State.OffY;
                            DVerts[v * 3 + 2] += State.OffZ;
                        }
                    }

                    if (AddedTile->offMeshCons && AddedTile->header->offMeshConCount > 0)
                    {
                        const int32 ConCount = AddedTile->header->offMeshConCount;
                        for (int32 c = 0; c < ConCount; ++c)
                        {
                            dtOffMeshConnection& Con =
                                const_cast<dtOffMeshConnection&>(AddedTile->offMeshCons[c]);
                            Con.pos[0] += State.OffX; Con.pos[1] += State.OffY; Con.pos[2] += State.OffZ;
                            Con.pos[3] += State.OffX; Con.pos[4] += State.OffY; Con.pos[5] += State.OffZ;
                        }
                    }
                }
            }

            const uint64 Ref64 = static_cast<uint64>(TileRef);
            State.AddedRefs.Add(Ref64);
            // addTile 成功のたびに TSet にも同時追加することで、
            // フェーズ完了時の一括構築(旧: 334ms スパイクの原因)を排除する
            State.AddedRefsSet.Add(Ref64);
        }

        State.NextTileIndex++;
        ProcessedThisFrame++;
    }

    // Reader の現在位置を保存 (次フレームの続き読み出しに使用)
    State.ReaderOffset = static_cast<int32>(Reader.Tell());

    // 進捗通知
    OnLoadProgress.Broadcast(State.NextTileIndex, State.Header.TileCount);

    // 全タイルの addTile が完了したら Relink フェーズへ移行
    if (State.NextTileIndex >= State.Header.TileCount)
    {
        State.TimeAfterTileAdd = FPlatformTime::Seconds();

        // ── Relink フェーズへ移行 ──
        // AddedRefsSet は各フレームの addTile 成功時に逐次追加済みのため
        // ここでの一括構築は不要。スパイクなしでそのまま Relink に進める。
        State.NextRelinkIndex = 0;
        State.Phase = FPendingLoadState::EPhase::Relink;

        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: [%s] TileAdd complete (%d tiles). Starting Relink phase."),
            *MakeTileKey(State.StageID, State.AgentName), State.AddedRefs.Num());
    }
}

// ──────────────────────────────────────────────────────────────────────
// Relink フェーズ
// フレーム分割追加では後から追加されたタイルが先のタイルへのリンクを
// 持っていないケースがある。追加済みタイルの隣接タイルを remove→re-add
// することで双方向リンクを確立する。
//
// 【旧実装の問題点】
//   FinalizePendingLoad 内で全タイルを一括処理していたため、
//   タイル数 N に対して O(N²) の処理がゲームスレッドを一括占有していた。
//   具体的には:
//     - TArray::Contains(NeighborRef64)  → O(N) 線形探索
//     - TArray::IndexOfByKey(NeighborRef64) → O(N) 線形探索
//   これらが外側の N ループの中で呼ばれるため O(N²) となり、
//   タイル数が数千規模で数十秒のフリーズが発生していた。
//
// 【修正内容】
//   1. TSet<uint64> を使い Contains を O(1) に改善
//   2. このフェーズ自体も TilesPerFrame 枚ずつ分割して実行
//   3. IndexOfByKey を廃止し TSet の Add/Remove で Ref を更新
// ──────────────────────────────────────────────────────────────────────

void ANavMeshSerializer::TickRelink(
    FPendingLoadState& State, dtNavMesh* DetourMesh, ARecastNavMesh* NavMesh)
{
    int32 ProcessedThisFrame = 0;
    const int32 TotalAdded = State.AddedRefs.Num();

    while (State.NextRelinkIndex < TotalAdded
        && ProcessedThisFrame < TilesPerFrame)
    {
        const uint64 CurRef64 = State.AddedRefs[State.NextRelinkIndex];

        const dtMeshTile* TileA = DetourMesh->getTileByRef(
            static_cast<dtTileRef>(CurRef64));

        if (TileA && TileA->header)
        {
            const int32 tx = TileA->header->x;
            const int32 ty = TileA->header->y;

            for (int32 dx = -1; dx <= 1; ++dx)
                for (int32 dy = -1; dy <= 1; ++dy)
                {
                    if (dx == 0 && dy == 0) continue;

                    const dtMeshTile* Neighbor = DetourMesh->getTileAt(tx + dx, ty + dy, 0);
                    if (!Neighbor || !Neighbor->header) continue;

                    const dtTileRef NeighborRef = DetourMesh->getTileRef(Neighbor);
                    if (NeighborRef == 0) continue;

                    const uint64 NeighborRef64 = static_cast<uint64>(NeighborRef);

                    // 隣接タイルが今回追加分でない場合はスキップ
                    // TSet::Contains は O(1) なので外側ループとの合計が O(N) になる
                    if (!State.AddedRefsSet.Contains(NeighborRef64)) continue;

                    // remove → re-add で双方向リンクを再構築
                    unsigned char* NeighborData = nullptr;
                    int32 NeighborDataSize = 0;
                    dtStatus RemoveStatus = DetourMesh->removeTile(
                        NeighborRef, &NeighborData, &NeighborDataSize);

                    if (dtStatusFailed(RemoveStatus) || !NeighborData) continue;

                    dtTileRef NewRef = 0;
                    dtStatus AddStatus = DetourMesh->addTile(
                        NeighborData, NeighborDataSize,
                        DT_TILE_FREE_DATA, 0, &NewRef);

                    if (dtStatusSucceed(AddStatus))
                    {
                        // TSet の Ref を古い値から新しい値に差し替える
                        // (IndexOfByKey による O(N) 線形探索を廃止)
                        State.AddedRefsSet.Remove(NeighborRef64);
                        State.AddedRefsSet.Add(static_cast<uint64>(NewRef));
                    }
                    else
                    {
                        dtFree(NeighborData, DT_ALLOC_TEMP);
                    }
                }
        }

        State.NextRelinkIndex++;
        ProcessedThisFrame++;
    }

    // Relink フェーズ完了
    if (State.NextRelinkIndex >= TotalAdded)
    {
        // TSet の最終状態を TArray に戻して LoadedTileRefs 登録に備える
        State.AddedRefs = State.AddedRefsSet.Array();

        FinalizePendingLoad(State, DetourMesh, NavMesh);
        PendingLoads.RemoveAt(0);
    }
}

// ──────────────────────────────────────────────────────────────────────
// 全フェーズ完了後の後処理
// NavigationSystem への通知・描画更新などを行う
//
// 【修正: FScene_AddPrimitive による 274ms スパイクの対策】
//   原因: RequestDrawingUpdate() がロードのたびに呼ばれ、
//         NavRenderingComp の FScene_AddPrimitive を誘発していた。
//   対策: ConditionalConstructGenerator と同様に、
//         全 PendingLoad 消化後 (最後の1件) のみ呼ぶよう変更。
//         10地形ロード時のスパイク発生回数が最大10回 → 1回に削減される。
// ──────────────────────────────────────────────────────────────────────

void ANavMeshSerializer::FinalizePendingLoad(
    FPendingLoadState& State, dtNavMesh* DetourMesh, ARecastNavMesh* NavMesh)
{
    const FString TileKey = MakeTileKey(State.StageID, State.AgentName);

    if (State.AddedRefs.Num() == 0)
    {
        UE_LOG(LogTemp, Error,
            TEXT("NavMeshSerializer: No tiles added for [%s]"), *TileKey);
        OnOperationFailed.Broadcast();
        return;
    }

    // ロード済みタイル管理に登録
    LoadedTileRefs.Add(TileKey, MoveTemp(State.AddedRefs));

    // UE NavigationSystem への通知
    UNavigationSystemV1* NavSys =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (NavSys)
    {
        NavSys->SetNavigationOctreeLock(true);
        NavSys->CancelBuild();
    }

    // ── ConditionalConstructGenerator / RequestDrawingUpdate の呼び出し判定 ──
    //
    // 【旧実装のバグ】
    //   PendingLoads.Num() <= 1 という条件は「キュー全体の残数」で判定していたため、
    //   Human と BigHuman を同時ロードした場合に以下の問題が発生していた:
    //
    //     PendingLoads: [Human, BigHuman]
    //     → Human 完了時:    Num()==2 → ConditionalConstructGenerator 呼ばれない
    //     → BigHuman 完了時: Num()==1 → BigHuman の NavMesh だけ呼ばれる
    //     → Human 側が描画されない (ロード→アンロード→ロードで再現)
    //
    // 【修正】
    //   「同じ AgentName のエントリがキューに自分以外で残っていないか」で判定する。
    //   これにより Human/BigHuman それぞれが独立して最後の1回を正しく検出できる。
    //   ※ PendingLoads[0] がまだ RemoveAt されていない自分自身なので &Other != &State で除外する
    const bool bIsLastForThisAgent = !PendingLoads.ContainsByPredicate(
        [&State](const FPendingLoadState& Other)
        {
            return (&Other != &State) && (Other.AgentName == State.AgentName);
        });

    if (bIsLastForThisAgent)
    {
        NavMesh->ConditionalConstructGenerator();

#if WITH_EDITOR
        NavMesh->RequestDrawingUpdate();
#else
        if (bIsVisualizationEnabled)
        {
            SetNavMeshVisualizationEnabled(false, State.AgentName);
            SetNavMeshVisualizationEnabled(true, State.AgentName);
        }
#endif
    }

    const double TimeEnd = FPlatformTime::Seconds();
    const double TotalMs = (TimeEnd - State.TimeStart) * 1000.0;
    const double FileLoadMs = (State.TimeAfterFileLoad - State.TimeStart) * 1000.0;
    const double TileAddMs = (State.TimeAfterTileAdd - State.TimeAfterFileLoad) * 1000.0;
    const double FinalizeMs = (TimeEnd - State.TimeAfterTileAdd) * 1000.0;

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: [LoadAndMergeNavTiles] Stage=[%s] Agent=[%s]\n")
        TEXT("  Total:      %.2f ms  (spread over frames)\n")
        TEXT("  FileLoad:   %.2f ms\n")
        TEXT("  TileAdd:    %.2f ms  (%d tiles, %d tiles/frame)\n")
        TEXT("  Finalize:   %.2f ms"),
        *State.StageID, *State.AgentName.ToString(),
        TotalMs,
        FileLoadMs,
        TileAddMs, State.Header.TileCount, TilesPerFrame,
        FinalizeMs
    );

    OnLoadComplete.Broadcast();
}

// ──────────────────────────────────────────────────────────────────────
// ロード・マージ (非同期ファイルIO + フレーム分割TileAdd + フレーム分割Relink)
// ──────────────────────────────────────────────────────────────────────

void ANavMeshSerializer::LoadAndMergeNavTiles(
    const FString& StageID, const FName& AgentName, FVector StageOffset)
{
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

    const double TimeStart = FPlatformTime::Seconds();

    // ── 非同期ファイルIO ──
    // ファイル読み込みをバックグラウンドスレッドで実行し、
    // 完了後にゲームスレッドで続きを処理する
    TSharedPtr<TArray<uint8>> FileDataPtr = MakeShared<TArray<uint8>>();

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
        [this, StageID, AgentName, StageOffset, FilePath, FileDataPtr, TimeStart]()
        {
            if (!FFileHelper::LoadFileToArray(*FileDataPtr, *FilePath))
            {
                AsyncTask(ENamedThreads::GameThread, [this]()
                    {
                        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: Failed to read file"));
                        OnOperationFailed.Broadcast();
                    });
                return;
            }

            const double TimeAfterFileLoad = FPlatformTime::Seconds();

            // ゲームスレッドでタイル追加処理に引き継ぎ
            AsyncTask(ENamedThreads::GameThread,
                [this, StageID, AgentName, StageOffset, FileDataPtr, TimeStart, TimeAfterFileLoad]()
                {
                    SetupPendingLoad(
                        StageID, AgentName, StageOffset,
                        MoveTemp(*FileDataPtr),
                        TimeStart, TimeAfterFileLoad
                    );
                });
        });
}

void ANavMeshSerializer::SetupPendingLoad(
    const FString& StageID,
    const FName& AgentName,
    FVector StageOffset,
    TArray<uint8>&& FileData,
    double TimeStart,
    double TimeAfterFileLoad)
{
    const FString TileKey = MakeTileKey(StageID, AgentName);

    ARecastNavMesh* NavMesh = GetRecastNavMeshForAgent(AgentName);
    if (!NavMesh) { OnOperationFailed.Broadcast(); return; }

    dtNavMesh* DetourMesh = NavMesh->GetRecastMesh();
    if (!DetourMesh)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: dtNavMesh is null"));
        OnOperationFailed.Broadcast();
        return;
    }

    // ── ファイル検証 ──
    FMemoryReader Reader(FileData);
    NavTileFile::FHeader Header;
    Reader.Serialize(&Header, sizeof(Header));

    if (Header.Magic != NavTileFile::Magic || Header.Version != NavTileFile::Version)
    {
        UE_LOG(LogTemp, Error, TEXT("NavMeshSerializer: Invalid file format"));
        OnOperationFailed.Broadcast();
        return;
    }

    // ── Params 不一致チェック → 再初期化 ──
    const dtNavMeshParams* CurrentParams = DetourMesh->getParams();
    if (CurrentParams->maxTiles != Header.NavMeshParams.maxTiles ||
        CurrentParams->maxPolys != Header.NavMeshParams.maxPolys)
    {
        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: Params mismatch. Reinitializing."));

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
                TEXT("NavMeshSerializer: dtNavMesh re-init failed (Status=0x%X)"), InitStatus);
            OnOperationFailed.Broadcast();
            return;
        }
    }

    // ── PendingLoad 状態を構築 ──
    FPendingLoadState State;
    State.StageID = StageID;
    State.AgentName = AgentName;
    State.FileData = MoveTemp(FileData);
    State.Header = Header;
    State.Phase = FPendingLoadState::EPhase::TileAdd;
    State.NextTileIndex = 0;
    State.ReaderOffset = static_cast<int32>(Reader.Tell()); // ヘッダ読み込み後の位置
    State.NextRelinkIndex = 0;
    State.TimeStart = TimeStart;
    State.TimeAfterFileLoad = TimeAfterFileLoad;

    // オフセット計算
    State.bHasOffset = !StageOffset.IsNearlyZero();
    if (State.bHasOffset)
    {
        State.OffX = static_cast<dtReal>(StageOffset.X);
        State.OffY = static_cast<dtReal>(StageOffset.Z);
        State.OffZ = static_cast<dtReal>(-StageOffset.Y);

        const dtNavMeshParams* Params = DetourMesh->getParams();
        State.TileOffsetX = FMath::RoundToInt(State.OffX / Params->tileWidth);
        State.TileOffsetY = FMath::RoundToInt(State.OffZ / Params->tileHeight);

        UE_LOG(LogTemp, Log,
            TEXT("NavMeshSerializer: TileOffset=(%d,%d)"),
            State.TileOffsetX, State.TileOffsetY);
    }

    // AddedRefsSet を事前確保しておく
    // TickTileAdd で逐次 Add するため、ここで Reserve することでリハッシュを防ぐ
    State.AddedRefsSet.Reserve(Header.TileCount);

    PendingLoads.Add(MoveTemp(State));

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Queued [%s] for frame-split loading (%d tiles, %d/frame)"),
        *TileKey, Header.TileCount, TilesPerFrame);
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

    // このエージェント用の SectionIndex を取得 or 新規割り当て
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

    // AgentName のハッシュから色を生成 (エージェントごとに異なる色)
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

// ──────────────────────────────────────────────────────────────────────
// 保存 (内部実装)
// ──────────────────────────────────────────────────────────────────────

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
// アンロード
// ──────────────────────────────────────────────────────────────────────

void ANavMeshSerializer::UnloadNavTiles(
    const FString& StageID, const FName& AgentName)
{
    const FString TileKey = MakeTileKey(StageID, AgentName);

    TArray<uint64>* Refs = LoadedTileRefs.Find(TileKey);
    if (!Refs || Refs->Num() == 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("NavMeshSerializer: [%s] is not loaded (or already queued for unload)."),
            *TileKey);
        return;
    }

    // 既にアンロードキューに入っていないか確認
    const bool bAlreadyQueued = PendingUnloads.ContainsByPredicate(
        [&TileKey, &StageID, &AgentName](const FPendingUnloadState& S)
        {
            return S.StageID == StageID && S.AgentName == AgentName;
        });
    if (bAlreadyQueued)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("NavMeshSerializer: [%s] is already queued for unload."),
            *TileKey);
        return;
    }

    FPendingUnloadState State;
    State.StageID = StageID;
    State.AgentName = AgentName;
    State.Refs = MoveTemp(*Refs);
    State.NextIndex = 0;

    LoadedTileRefs.Remove(TileKey);

    UE_LOG(LogTemp, Log,
        TEXT("NavMeshSerializer: Queued [%s] for frame-split unloading (%d tiles)"),
        *TileKey, State.Refs.Num());

    PendingUnloads.Add(MoveTemp(State));
}