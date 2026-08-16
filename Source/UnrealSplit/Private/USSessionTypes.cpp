// Copyright (c) Jonas Itt. All Rights Reserved.

#include "USSessionTypes.h"

namespace
{
	constexpr TCHAR SlotDelimiter = TEXT('\x1f');
	constexpr TCHAR ExtraPairDelimiter = TEXT('\x1f');
	constexpr TCHAR ExtraKeyValueDelimiter = TEXT('\x1e');

	FString SanitizeAdvertisedToken(const FString& Token)
	{
		FString Clean = Token;
		Clean.ReplaceCharInline(SlotDelimiter, TEXT('_'));
		Clean.ReplaceCharInline(ExtraKeyValueDelimiter, TEXT('_'));
		return Clean;
	}

	FString JoinWithDelimiter(const TArray<FString>& Parts, TCHAR Delimiter)
	{
		FString Result;
		for (int32 Index = 0; Index < Parts.Num(); ++Index)
		{
			if (Index > 0)
			{
				Result.AppendChar(Delimiter);
			}
			Result += Parts[Index];
		}
		return Result;
	}
}

TArray<FString> FUSAdvertisedSessionState::GetVacantLabels() const
{
	TArray<FString> Vacant;

	if (Phase != EUSAdvertisedSessionPhase::InProgress)
	{
		return Vacant;
	}

	for (const FUSAdvertisedSlot& Slot : Slots)
	{
		if (!Slot.bOccupied && !Slot.Label.IsEmpty())
		{
			Vacant.Add(Slot.Label);
		}
	}

	return Vacant;
}

FString FUSAdvertisedSessionState::GetFindListSummary() const
{
	switch (Phase)
	{
	case EUSAdvertisedSessionPhase::Lobby:
		return TEXT("Lobby");

	case EUSAdvertisedSessionPhase::InProgress:
	{
		const TArray<FString> Vacant = GetVacantLabels();
		if (Vacant.Num() == 0)
		{
			return TEXT("In progress");
		}

		FString Summary;
		for (int32 Index = 0; Index < Vacant.Num(); ++Index)
		{
			if (Index > 0)
			{
				Summary += TEXT(", ");
			}
			Summary += Vacant[Index];
			Summary += TEXT(" missing");
		}
		return Summary;
	}

	default:
		return FString();
	}
}

void FUSAdvertisedSessionState::Pack(int32& OutPhase, FString& OutLabels, int32& OutOccupied, FString& OutExtra) const
{
	OutPhase = static_cast<int32>(Phase);

	TArray<FString> Labels;
	int32 OccupiedMask = 0;

	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		Labels.Add(SanitizeAdvertisedToken(Slots[Index].Label));
		if (Slots[Index].bOccupied && Index < 32)
		{
			OccupiedMask |= (1 << Index);
		}
	}

	OutLabels = JoinWithDelimiter(Labels, SlotDelimiter);
	OutOccupied = OccupiedMask;

	TArray<FString> ExtraKeys;
	Extra.GetKeys(ExtraKeys);
	ExtraKeys.Sort();

	TArray<FString> ExtraPairs;
	for (const FString& Key : ExtraKeys)
	{
		const FString* const Value = Extra.Find(Key);
		FString Pair = SanitizeAdvertisedToken(Key);
		Pair.AppendChar(ExtraKeyValueDelimiter);
		Pair += SanitizeAdvertisedToken(Value != nullptr ? *Value : FString());
		ExtraPairs.Add(Pair);
	}

	OutExtra = JoinWithDelimiter(ExtraPairs, ExtraPairDelimiter);
}

FUSAdvertisedSessionState FUSAdvertisedSessionState::Unpack(int32 Phase, const FString& Labels, int32 Occupied, const FString& ExtraPacked)
{
	FUSAdvertisedSessionState State;

	if (Phase == static_cast<int32>(EUSAdvertisedSessionPhase::Lobby))
	{
		State.Phase = EUSAdvertisedSessionPhase::Lobby;
	}
	else if (Phase == static_cast<int32>(EUSAdvertisedSessionPhase::InProgress))
	{
		State.Phase = EUSAdvertisedSessionPhase::InProgress;
	}
	else
	{
		State.Phase = EUSAdvertisedSessionPhase::Unknown;
	}

	if (State.Phase == EUSAdvertisedSessionPhase::Unknown && Labels.IsEmpty() && Occupied == 0 && ExtraPacked.IsEmpty())
	{
		return State;
	}

	TArray<FString> LabelParts;
	if (!Labels.IsEmpty())
	{
		Labels.ParseIntoArray(LabelParts, TEXT("\x1f"), false);
	}

	for (int32 Index = 0; Index < LabelParts.Num(); ++Index)
	{
		FUSAdvertisedSlot Slot;
		Slot.Label = LabelParts[Index];
		Slot.bOccupied = (Occupied & (1 << Index)) != 0;
		State.Slots.Add(Slot);
	}

	if (!ExtraPacked.IsEmpty())
	{
		TArray<FString> Pairs;
		ExtraPacked.ParseIntoArray(Pairs, TEXT("\x1f"), true);

		for (const FString& Pair : Pairs)
		{
			FString Key;
			FString Value;
			if (Pair.Split(TEXT("\x1e"), &Key, &Value))
			{
				State.Extra.Add(Key, Value);
			}
		}
	}

	return State;
}
