// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class SaveLoadNavimeshFile : ModuleRules
{
    public SaveLoadNavimeshFile(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "NavigationSystem", "AIModule", "Navmesh", "ProceduralMeshComponent", });
    }
}
