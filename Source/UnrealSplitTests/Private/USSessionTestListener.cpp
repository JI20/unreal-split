// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#include "USSessionTestListener.h"

#include "USSessionSubsystem.h"

void UUSSessionTestListener::ResetHostFlags()
{
	bHostSeen = false;
	bHostOk = false;
}

void UUSSessionTestListener::ResetJoinFlags()
{
	bJoinSeen = false;
	bJoinOk = false;
}

void UUSSessionTestListener::BindHost(UUSSessionSubsystem* Sessions)
{
	if (Sessions == nullptr)
	{
		return;
	}

	if (bHostBound)
	{
		Sessions->OnSessionHostedDelegate.RemoveDynamic(this, &UUSSessionTestListener::HandleHosted);
		bHostBound = false;
	}

	ResetHostFlags();
	Sessions->OnSessionHostedDelegate.AddDynamic(this, &UUSSessionTestListener::HandleHosted);
	bHostBound = true;
}

void UUSSessionTestListener::BindJoin(UUSSessionSubsystem* Sessions)
{
	if (Sessions == nullptr)
	{
		return;
	}

	if (bJoinBound)
	{
		Sessions->OnJoinCompletedDelegate.RemoveDynamic(this, &UUSSessionTestListener::HandleJoin);
		bJoinBound = false;
	}

	ResetJoinFlags();
	Sessions->OnJoinCompletedDelegate.AddDynamic(this, &UUSSessionTestListener::HandleJoin);
	bJoinBound = true;
}

void UUSSessionTestListener::UnbindAll(UUSSessionSubsystem* Sessions)
{
	if (Sessions != nullptr)
	{
		if (bHostBound)
		{
			Sessions->OnSessionHostedDelegate.RemoveDynamic(this, &UUSSessionTestListener::HandleHosted);
		}
		if (bJoinBound)
		{
			Sessions->OnJoinCompletedDelegate.RemoveDynamic(this, &UUSSessionTestListener::HandleJoin);
		}
	}

	bHostBound = false;
	bJoinBound = false;
}

void UUSSessionTestListener::HandleHosted(bool bSuccess)
{
	bHostSeen = true;
	bHostOk = bSuccess;
}

void UUSSessionTestListener::HandleJoin(bool bSuccess)
{
	bJoinSeen = true;
	bJoinOk = bSuccess;
}
