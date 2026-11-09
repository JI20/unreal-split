// Copyright 2026 Jonas Itt
// SPDX-License-Identifier: Apache-2.0

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "USSessionTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUSAdvertisedSessionPackRoundTripTest,
	"UnrealSplit.Session.Advertisement.PackRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FUSAdvertisedSessionPackRoundTripTest::RunTest(const FString& Parameters)
{
	FUSAdvertisedSessionState Empty = FUSAdvertisedSessionState::Unpack(0, FString(), 0, FString());
	TestEqual(TEXT("Empty unpack is Unknown."), Empty.Phase, EUSAdvertisedSessionPhase::Unknown);
	TestEqual(TEXT("Empty unpack has no slots."), Empty.Slots.Num(), 0);
	TestTrue(TEXT("Unknown Find summary is empty."), Empty.GetFindListSummary().IsEmpty());

	FUSAdvertisedSessionState Lobby;
	Lobby.Phase = EUSAdvertisedSessionPhase::Lobby;
	FUSAdvertisedSlot Elmar;
	Elmar.Label = TEXT("Elmar");
	FUSAdvertisedSlot Kit;
	Kit.Label = TEXT("Kit");
	Lobby.Slots.Add(Elmar);
	Lobby.Slots.Add(Kit);

	int32 Phase = 0;
	FString Labels;
	int32 Occupied = 0;
	FString Extra;
	Lobby.Pack(Phase, Labels, Occupied, Extra);

	const FUSAdvertisedSessionState LobbyBack = FUSAdvertisedSessionState::Unpack(Phase, Labels, Occupied, Extra);
	TestEqual(TEXT("Lobby phase survives."), LobbyBack.Phase, EUSAdvertisedSessionPhase::Lobby);
	TestEqual(TEXT("Lobby keeps two slots."), LobbyBack.Slots.Num(), 2);
	TestEqual(TEXT("Lobby slot 0 label."), LobbyBack.Slots[0].Label, FString(TEXT("Elmar")));
	TestEqual(TEXT("Lobby slot 1 label."), LobbyBack.Slots[1].Label, FString(TEXT("Kit")));
	TestEqual(TEXT("Lobby Find summary."), LobbyBack.GetFindListSummary(), FString(TEXT("Lobby")));
	TestEqual(TEXT("Lobby has no vacant labels."), LobbyBack.GetVacantLabels().Num(), 0);

	FUSAdvertisedSessionState Match;
	Match.Phase = EUSAdvertisedSessionPhase::InProgress;
	Elmar.bOccupied = true;
	Kit.bOccupied = false;
	Match.Slots.Add(Elmar);
	Match.Slots.Add(Kit);
	Match.Extra.Add(TEXT("puzzle"), TEXT("harbor"));

	Match.Pack(Phase, Labels, Occupied, Extra);
	const FUSAdvertisedSessionState MatchBack = FUSAdvertisedSessionState::Unpack(Phase, Labels, Occupied, Extra);
	TestEqual(TEXT("Match phase survives."), MatchBack.Phase, EUSAdvertisedSessionPhase::InProgress);
	TestTrue(TEXT("Elmar occupied."), MatchBack.Slots[0].bOccupied);
	TestFalse(TEXT("Kit vacant."), MatchBack.Slots[1].bOccupied);
	TestEqual(TEXT("Match Find summary."), MatchBack.GetFindListSummary(), FString(TEXT("Kit missing")));
	TestEqual(TEXT("Extra puzzle."), MatchBack.Extra.FindRef(TEXT("puzzle")), FString(TEXT("harbor")));

	FUSAdvertisedSessionState Dirty;
	Dirty.Phase = EUSAdvertisedSessionPhase::InProgress;
	FUSAdvertisedSlot DirtySlot;
	DirtySlot.Label = FString::Printf(TEXT("Na%cme"), TCHAR(0x1f));
	Dirty.Slots.Add(DirtySlot);
	Dirty.Pack(Phase, Labels, Occupied, Extra);
	const FUSAdvertisedSessionState DirtyBack = FUSAdvertisedSessionState::Unpack(Phase, Labels, Occupied, Extra);
	TestTrue(TEXT("Delimiter in a label is stripped."), !DirtyBack.Slots[0].Label.Contains(TEXT("\x1f")));

	return true;
}

#endif
