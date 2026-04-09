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

    //----------------------------------------------------------------------//
    // 公開API（全エージェント一括操作）
    //----------------------------------------------------------------------//

    /** 全エージェントのNavMeshを一括保存（ビルド完了待ちあり） */
    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    void SaveNavMeshWhenReady(const FString& StageID);

    /** 全エージェントのNavMeshを一括ロード・適用 */
    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    void LoadAndApplyNavMesh(const FString& StageID, FVector StageOffset = FVector::ZeroVector);

    /** 指定StageIDの保存データが（1つ以上）存在するか */
    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    bool HasSavedNavMesh(const FString& StageID) const;

    /** 指定エージェントのNavMesh可視化を切り替える */
    UFUNCTION(BlueprintCallable, Category = "NavMesh|Debug")
    void SetNavMeshVisualizationEnabled(bool bEnabled, FName AgentName);

    /** 指定エージェントのNavMesh可視化が有効か */
    UFUNCTION(BlueprintCallable, Category = "NavMesh|Debug")
    bool IsNavMeshVisualizationEnabled(FName AgentName) const;

    /** 指定エージェントのNavMeshだけ動的リビルドを有効化 */
    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    void EnableNavMeshDynamicRebuild(FName AgentName);

    /** 全エージェントの動的リビルドを有効化 */
    UFUNCTION(BlueprintCallable, Category = "NavMesh")
    void EnableNavMeshDynamicRebuildAll();

    //----------------------------------------------------------------------//
    // ビルド進捗モニタリング
    //----------------------------------------------------------------------//

    /** ビルド進捗の画面表示を開始する。
     *  各エージェントの進捗がPrintStringで画面左上に表示される。
     *  残りビルドタスク数ベースで進捗率を算出。
     *  全エージェントのビルドが完了すると自動停止。 */
    UFUNCTION(BlueprintCallable, Category = "NavMesh|Debug")
    void StartBuildProgressMonitor();

    /** ビルド進捗の画面表示を停止する */
    UFUNCTION(BlueprintCallable, Category = "NavMesh|Debug")
    void StopBuildProgressMonitor();

    //----------------------------------------------------------------------//
    // デリゲート
    //----------------------------------------------------------------------//

    UPROPERTY(BlueprintAssignable, Category = "NavMesh")
    FOnNavMeshSaveComplete OnSaveComplete;

    UPROPERTY(BlueprintAssignable, Category = "NavMesh")
    FOnNavMeshLoadComplete OnLoadComplete;

    UPROPERTY(BlueprintAssignable, Category = "NavMesh")
    FOnNavMeshOperationFailed OnOperationFailed;

    //----------------------------------------------------------------------//
    // BP設定
    //----------------------------------------------------------------------//

    /** エージェント名ごとの可視化マテリアル */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NavMesh|Debug")
    TMap<FName, TObjectPtr<UMaterialInterface>> AgentVisMaterials;

    /** 非同期ビルド時の同時タイル生成ジョブ数上限。
     *  大きくするほどCPUコアを多く使いビルドが高速化する。
     *  0の場合はNavMeshのデフォルト値をそのまま使用。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NavMesh|Build", meta = (ClampMin = "0", UIMin = "0"))
    int32 MaxTileJobsCount = 0;

private:
    //----------------------------------------------------------------------//
    // 内部ヘルパー
    //----------------------------------------------------------------------//

    TArray<ARecastNavMesh*> GetAllRecastNavMeshes() const;
    ARecastNavMesh* FindNavMeshByAgentName(FName AgentName) const;
    static FName GetAgentNameFromNavMesh(const ARecastNavMesh* NavMesh);
    FString GetNavDataFilePath(const FString& StageID, const FString& AgentName) const;
    void DisableNavMeshAutoRebuild() const;

    //----------------------------------------------------------------------//
    // 保存
    //----------------------------------------------------------------------//

    void SaveAllNavMeshes(const FString& StageID);
    bool SaveSingleNavMesh(ARecastNavMesh* NavMesh, const FString& FilePath);

    UFUNCTION()
    void OnNavBuildFinished(ANavigationData* NavData);

    FString PendingSaveStageID;

    //----------------------------------------------------------------------//
    // ロード
    //----------------------------------------------------------------------//

    void LoadAndApplyAllNavMeshes(const FString& StageID, FVector StageOffset);
    bool LoadSingleNavMesh(ARecastNavMesh* NavMesh, const FString& FilePath, FVector StageOffset);

    FTimerHandle RegistrationWaitHandle;
    void OnNavDataRegistrationComplete();
    int32 PendingRegistrationCount = 0;

    //----------------------------------------------------------------------//
    // 可視化
    //----------------------------------------------------------------------//

    void BuildNavMeshVisualizationForAgent(ARecastNavMesh* NavMesh);
    void ClearNavMeshVisualizationForAgent(FName AgentName);

    UPROPERTY()
    TMap<FName, TObjectPtr<UProceduralMeshComponent>> NavMeshVisComponents;

    TSet<FName> EnabledVisAgents;

    //----------------------------------------------------------------------//
    // ビルド進捗モニタリング
    //----------------------------------------------------------------------//

    virtual void Tick(float DeltaTime) override;

    /** 進捗モニタ用: エージェントごとの初期タスク数（モニタ開始時にスナップショット） */
    TMap<FName, int32> InitialBuildTaskCounts;

    /** 進捗モニタ用: エージェントごとのPrintString用Key */
    TMap<FName, int32> AgentDebugMsgKeys;

    bool bMonitoringBuildProgress = false;

    /** 初期タスク数が確定するまでの待ちフレーム（RebuildAll直後は0の場合がある） */
    int32 MonitorWarmupFrames = 0;

    int32 NextDebugMsgKey = 100;
};