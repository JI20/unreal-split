// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "USSessionTestListener.generated.h"

class UUSSessionSubsystem;

/**
 * UObject sink for UUSSessionSubsystem dynamic multicast delegates.
 * Dynamic delegates have no AddLambda — tests bind via AddDynamic.
 */
UCLASS()
class UUSSessionTestListener : public UObject
{
	GENERATED_BODY()

public:
	bool bHostSeen = false;
	bool bHostOk = false;
	bool bJoinSeen = false;
	bool bJoinOk = false;

	bool bHostBound = false;
	bool bJoinBound = false;

	void ResetHostFlags();
	void ResetJoinFlags();

	void BindHost(UUSSessionSubsystem* Sessions);
	void BindJoin(UUSSessionSubsystem* Sessions);
	void UnbindAll(UUSSessionSubsystem* Sessions);

	UFUNCTION()
	void HandleHosted(bool bSuccess);

	UFUNCTION()
	void HandleJoin(bool bSuccess);
};
