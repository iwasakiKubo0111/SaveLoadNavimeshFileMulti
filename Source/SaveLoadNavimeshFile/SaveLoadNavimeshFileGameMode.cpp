// Copyright Epic Games, Inc. All Rights Reserved.

#include "SaveLoadNavimeshFileGameMode.h"
#include "SaveLoadNavimeshFileCharacter.h"
#include "UObject/ConstructorHelpers.h"

ASaveLoadNavimeshFileGameMode::ASaveLoadNavimeshFileGameMode()
{
	// set default pawn class to our Blueprinted character
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnBPClass(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"));
	if (PlayerPawnBPClass.Class != NULL)
	{
		DefaultPawnClass = PlayerPawnBPClass.Class;
	}
}
