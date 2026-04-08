#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "NavMesh/RecastNavMesh.h"
#include "ProceduralMeshComponent.h"
#include "Detour/DetourNavMesh.h"

// ── Unreal Insights トレース用ヘッダ ──────────────────────────────────
// CPU プロファイリングチャンネル を有効化するために必要
#include "ProfilingDebugging/CpuProfilerTrace.h"
// ─────────────────────────────────────────────────────────────────────

#include "NavMeshSerializer.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnNavMeshSaveComplete);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnNavMeshLoadComplete);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnNavMeshOperationFailed);

// フレーム分割ロードの進捗を通知するデリゲート
// 引数: 完了タイル数, 総タイル数
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnNavMeshLoadProgress, int32, LoadedTiles, int32, TotalTiles);

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

UCLASS()
class SAVELOADNAVIMESHFILE_API ANavMeshSerializer : public AActor
{
    GENERATED_BODY()

public:
    ANavMeshSerializer();

    // ──────────────────────────────────────────
    // 保存
    // ──────────────────────────────────────────

    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    void SaveNavTilesWhenReady(const FString& StageID, const FName& AgentName = NAME_None);

    // ──────────────────────────────────────────
    // ロード
    // ──────────────────────────────────────────

    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    void LoadAndMergeNavTiles(const FString& StageID, const FName& AgentName = NAME_None, FVector StageOffset = FVector::ZeroVector);

    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    void UnloadNavTiles(const FString& StageID, const FName& AgentName = NAME_None);

    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    TArray<FString> GetLoadedStageIDs() const;

    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    bool HasSavedNavMesh(const FString& StageID, const FName& AgentName = NAME_None) const;

    // ──────────────────────────────────────────
    // 可視化
    // ──────────────────────────────────────────

    UFUNCTION(BlueprintCallable, Category = "NavMesh|Debug")
    void SetNavMeshVisualizationEnabled(bool bEnabled, const FName& AgentName = NAME_None);

    UFUNCTION(BlueprintCallable, Category = "NavMesh|Debug")
    bool IsNavMeshVisualizationEnabled() const { return bIsVisualizationEnabled; }

    UFUNCTION(BlueprintCallable, Category = "SetViewMode|Debug")
    void SetViewMode(EViewModeIndex ViewMode);

    // ──────────────────────────────────────────
    // デリゲート
    // ──────────────────────────────────────────

    UPROPERTY(BlueprintAssignable, Category = "NavMesh")
    FOnNavMeshSaveComplete OnSaveComplete;

    UPROPERTY(BlueprintAssignable, Category = "NavMesh")
    FOnNavMeshLoadComplete OnLoadComplete;

    UPROPERTY(BlueprintAssignable, Category = "NavMesh")
    FOnNavMeshOperationFailed OnOperationFailed;

    UPROPERTY(BlueprintAssignable, Category = "NavMesh")
    FOnNavMeshLoadProgress OnLoadProgress;

    // ──────────────────────────────────────────
    // エディタ設定
    // ──────────────────────────────────────────

    // エージェント名 → マテリアルのMap
    // キー: AgentName (例: "Human", "BigHuman")
    // 値:   使用するマテリアル
    // NAME_None キーはAgentName未指定時のデフォルトとして使用
    UPROPERTY(EditAnywhere, Category = "NavMesh|Debug")
    TMap<FName, TObjectPtr<UMaterialInterface>> NavVisMaterialMap;

    // 1フレームあたりに処理するタイル数 (エディタから調整可能)
    UPROPERTY(EditAnywhere, Category = "NavMesh", meta = (ClampMin = "1", ClampMax = "500"))
    int32 TilesPerFrame = 1000;

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;

private:
    // ── 内部実装 ──
    void SaveNavTiles(const FString& StageID, const FName& AgentName);
    ARecastNavMesh* GetRecastNavMeshForAgent(const FName& AgentName) const;
    FString GetNavDataFilePath(const FString& StageID, const FName& AgentName) const;
    static FString MakeTileKey(const FString& StageID, const FName& AgentName);

    void BuildNavMeshVisualization(const FName& AgentName);
    void ClearNavMeshVisualization(const FName& AgentName = NAME_None);

    UFUNCTION()
    void OnNavBuildFinished(ANavigationData* NavData);

    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    void EnableNavMeshDynamicRebuild();

    // ── ビルド完了待ち保留データ ──
    FString PendingSaveStageID;
    FName   PendingSaveAgentName;

    // ── フレーム分割ロード用の内部状態 ──
    struct FPendingLoadState
    {
        // ロード対象の識別情報
        FString                 StageID;
        FName                   AgentName;

        // ファイルデータ・ヘッダ
        TArray<uint8>           FileData;
        NavTileFile::FHeader    Header;

        // ── フェーズ管理 ──
        // TileAdd  : addTile を TilesPerFrame 枚ずつ実行するフェーズ
        // Relink   : 追加済みタイル間の境界リンクを TilesPerFrame 枚ずつ再構築するフェーズ
        //            (フレーム分割追加では後から追加されたタイルが先のタイルへの
        //             リンクを持たないため、このフェーズで双方向リンクを確立する)
        // Done     : 使用しない (RemoveAt 済みを示す番兵)
        enum class EPhase : uint8 { TileAdd, Relink } Phase = EPhase::TileAdd;

        // ── TileAdd フェーズ用 ──
        int32                   NextTileIndex = 0;  // 次に処理するタイルインデックス
        int32                   ReaderOffset = 0;  // FMemoryReader のシーク位置

        // ── Relink フェーズ用 ──
        // O(1) な Contains を実現するための Set (TArray::Contains は O(N) で致命的に遅い)
        TSet<uint64>            AddedRefsSet;
        // Relink で処理する AddedRefs の次インデックス
        int32                   NextRelinkIndex = 0;

        // ── 共通 ──
        // ロード完了後に LoadedTileRefs へ登録する Ref 一覧
        // Relink フェーズ完了時に AddedRefsSet から再構築される
        TArray<uint64>          AddedRefs;

        // オフセット情報
        bool                    bHasOffset = false;
        dtReal                  OffX = 0, OffY = 0, OffZ = 0;
        int32                   TileOffsetX = 0, TileOffsetY = 0;

        // 計測用タイムスタンプ
        double                  TimeStart = 0.0;
        double                  TimeAfterFileLoad = 0.0;
        double                  TimeAfterTileAdd = 0.0;
    };

    TArray<FPendingLoadState> PendingLoads;

    struct FPendingUnloadState
    {
        FString   StageID;
        FName     AgentName;
        TArray<uint64> Refs;       // removeTile 対象の TileRef 一覧
        int32     NextIndex = 0;   // 次に処理する Refs のインデックス
    };

    TArray<FPendingUnloadState> PendingUnloads;

    // SetupPendingLoad: ファイル検証・Params チェック・State 構築を行い PendingLoads に追加
    void SetupPendingLoad(const FString& StageID, const FName& AgentName,
        FVector StageOffset, TArray<uint8>&& FileData,
        double TimeStart, double TimeAfterFileLoad);

    // Tick から呼ばれるフレーム分割処理ディスパッチャ
    void TickPendingLoads();
    void TickPendingUnloads();

    // TileAdd フェーズ: addTile を TilesPerFrame 枚処理する
    void TickTileAdd(FPendingLoadState& State, dtNavMesh* DetourMesh);

    // Relink フェーズ: 追加済みタイル間の境界リンクを TilesPerFrame 枚処理する
    void TickRelink(FPendingLoadState& State, dtNavMesh* DetourMesh, ARecastNavMesh* NavMesh);

    // 全フェーズ完了後の後処理 (NavigationSystem 通知・描画更新など)
    void FinalizePendingLoad(FPendingLoadState& State, dtNavMesh* DetourMesh, ARecastNavMesh* NavMesh);

    // ── ロード済みタイル管理 ──
    // キー = "StageID_AgentName" 形式
    TMap<FString, TArray<uint64>> LoadedTileRefs;

    // ── 可視化 ──
    UPROPERTY()
    UProceduralMeshComponent* NavMeshVisMesh = nullptr;

    // エージェント名 → MeshSectionIndex
    TMap<FName, int32> AgentVisSectionMap;

    // 次に割り当てるSectionIndex
    int32 NextVisSectionIndex = 0;

    bool bIsVisualizationEnabled = false;

    // SetViewMode用: 元のShowFlagsを保存しておく
    TOptional<FEngineShowFlags> SavedShowFlags;
    TOptional<EViewModeIndex>   SavedViewModeIndex;
};