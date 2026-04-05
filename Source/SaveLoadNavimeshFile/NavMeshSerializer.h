#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "NavMesh/RecastNavMesh.h"
#include "ProceduralMeshComponent.h"
#include "NavMeshSerializer.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnNavMeshSaveComplete);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnNavMeshLoadComplete);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnNavMeshOperationFailed);

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

    // ──────────────────────────────────────────
    // エディタ設定
    // ──────────────────────────────────────────

    // エージェント名 → マテリアルのMap
    // キー: AgentName (例: "Human", "BigHuman")
    // 値:   使用するマテリアル
    // NAME_None キーはAgentName未指定時のデフォルトとして使用
    UPROPERTY(EditAnywhere, Category = "NavMesh|Debug")
    TMap<FName, TObjectPtr<UMaterialInterface>> NavVisMaterialMap;

protected:
    virtual void BeginPlay() override;

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