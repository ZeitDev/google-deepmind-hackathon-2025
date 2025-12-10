// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NodeDumperLibrary.generated.h"

/**
 * 
 */
UCLASS()
class MYPROJECT_API UNodeDumperLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

	public:
		// Call this from a Blutility or Editor Utility Widget
		UFUNCTION(BlueprintCallable, Category = "Hackathon Tools")
		static void DumpAllNodes(FString FilePath);
	
};
