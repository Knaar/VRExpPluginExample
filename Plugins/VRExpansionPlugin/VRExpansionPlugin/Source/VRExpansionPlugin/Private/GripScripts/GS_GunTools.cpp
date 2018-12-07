// Fill out your copyright notice in the Description page of Project Settings.

#include "GripScripts/GS_GunTools.h"
#include "VRGripInterface.h"
#include "GripMotionControllerComponent.h"

UGS_GunTools::UGS_GunTools(const FObjectInitializer& ObjectInitializer) :
	Super(ObjectInitializer)
{
	bIsActive = true;
	WorldTransformOverrideType = EGSTransformOverrideType::OverridesWorldTransform;

	PivotOffset = FVector::ZeroVector;
	ShoulderMountComponent = nullptr;
	ShoulderMountRelativeTransform = FTransform::Identity;
	ShoulderMountSocketOverride = NAME_None;


	bHasRecoil = false;
	MaxRecoilTranslation = FVector::ZeroVector;
	MaxRecoilRotation = FVector::ZeroVector;
	MaxRecoilScale = FVector::ZeroVector;
	DecayRate = 1.f;

	BackEndRecoilStorage = FTransform::Identity;

	ShoulderSnapDistance = 100.f;
	bUseDistanceBasedShoulderSnapping = true;
}

bool UGS_GunTools::GetWorldTransform_Implementation
(
	UGripMotionControllerComponent* GrippingController, 
	float DeltaTime, FTransform & WorldTransform, 
	const FTransform &ParentTransform, 
	FBPActorGripInformation &Grip, 
	AActor * actor, 
	UPrimitiveComponent * root, 
	bool bRootHasInterface, 
	bool bActorHasInterface, 
	bool bIsForTeleport
) 
{
	if (!GrippingController)
		return false;

	// Just simple transform setting
	if (bHasRecoil)
	{
		BackEndRecoilStorage.Blend(BackEndRecoilStorage, BackEndRecoilTarget, FMath::Clamp(LerpRate * DeltaTime, 0.f, 1.f));
		BackEndRecoilTarget.Blend(BackEndRecoilTarget, FTransform::Identity, FMath::Clamp(DecayRate * DeltaTime, 0.f, 1.f));
		WorldTransform = Grip.RelativeTransform * Grip.AdditionTransform * BackEndRecoilStorage * ParentTransform;
	}
	else
		WorldTransform = Grip.RelativeTransform * Grip.AdditionTransform * ParentTransform;

	// Check the grip lerp state, this it ouside of the secondary attach check below because it can change the result of it
	if ((Grip.SecondaryGripInfo.bHasSecondaryAttachment && Grip.SecondaryGripInfo.SecondaryAttachment) || Grip.SecondaryGripInfo.GripLerpState == EGripLerpState::EndLerp)
	{
		switch (Grip.SecondaryGripInfo.GripLerpState)
		{
		case EGripLerpState::StartLerp:
		case EGripLerpState::EndLerp:
		{
			if (Grip.SecondaryGripInfo.curLerp > 0.01f)
				Grip.SecondaryGripInfo.curLerp -= DeltaTime;
			else
			{
				if (Grip.SecondaryGripInfo.bHasSecondaryAttachment &&
					Grip.AdvancedGripSettings.SecondaryGripSettings.bUseSecondaryGripSettings &&
					Grip.AdvancedGripSettings.SecondaryGripSettings.SecondaryGripScaler < 1.0f)
				{
					Grip.SecondaryGripInfo.GripLerpState = EGripLerpState::ConstantLerp;
				}
				else
					Grip.SecondaryGripInfo.GripLerpState = EGripLerpState::NotLerping;
			}

		}break;
		case EGripLerpState::ConstantLerp:
		case EGripLerpState::NotLerping:
		default:break;
		}
	}

	// Handle the interp and multi grip situations, re-checking the grip situation here as it may have changed in the switch above.
	if ((Grip.SecondaryGripInfo.bHasSecondaryAttachment && Grip.SecondaryGripInfo.SecondaryAttachment) || Grip.SecondaryGripInfo.GripLerpState == EGripLerpState::EndLerp)
	{
		FTransform SecondaryTransform = Grip.RelativeTransform * ParentTransform;

		// Checking secondary grip type for the scaling setting
		ESecondaryGripType SecondaryType = ESecondaryGripType::SG_None;

		if (bRootHasInterface)
			SecondaryType = IVRGripInterface::Execute_SecondaryGripType(root);
		else if (bActorHasInterface)
			SecondaryType = IVRGripInterface::Execute_SecondaryGripType(actor);

		// If the grip is a custom one, skip all of this logic we won't be changing anything
		if (SecondaryType != ESecondaryGripType::SG_Custom)
		{
			// Variables needed for multi grip transform
			FVector BasePoint;
			FVector Pivot;

			if (ShoulderMountComponent.IsValid())
			{
				BasePoint = ShoulderMountComponent->GetComponentTransform().GetLocation();
				Pivot = (FTransform(PivotOffset) * ShoulderMountComponent->GetComponentTransform()).GetLocation();
				SecondaryTransform = FTransform(PivotOffset) * SecondaryTransform;
			}
			else
			{
				BasePoint = ParentTransform.GetLocation();
				Pivot = (FTransform(PivotOffset) * ParentTransform).GetLocation();
				SecondaryTransform = FTransform(PivotOffset) * SecondaryTransform;
			}
				
			const FTransform PivotToWorld = FTransform(FQuat::Identity, Pivot);//BasePoint);
			const FTransform WorldToPivot = FTransform(FQuat::Identity, -Pivot);//-BasePoint);

			FVector frontLocOrig;
			FVector frontLoc;

			// Ending lerp out of a multi grip
			if (Grip.SecondaryGripInfo.GripLerpState == EGripLerpState::EndLerp)
			{
				frontLocOrig = (/*WorldTransform*/SecondaryTransform.TransformPosition(Grip.SecondaryGripInfo.SecondaryRelativeTransform.GetLocation())) - BasePoint;
				frontLoc = Grip.SecondaryGripInfo.LastRelativeLocation;

				frontLocOrig = FMath::Lerp(frontLoc, frontLocOrig, FMath::Clamp(Grip.SecondaryGripInfo.curLerp / Grip.SecondaryGripInfo.LerpToRate, 0.0f, 1.0f));
			}
			else // Is in a multi grip, might be lerping into it as well.
			{
				//FVector curLocation; // Current location of the secondary grip

				bool bPulledControllerLoc = false;
				if (GrippingController->bHasAuthority && Grip.SecondaryGripInfo.SecondaryAttachment->GetOwner() == GrippingController->GetOwner())
				{
					if (UGripMotionControllerComponent * OtherController = Cast<UGripMotionControllerComponent>(Grip.SecondaryGripInfo.SecondaryAttachment))
					{
						if (!OtherController->bUseWithoutTracking)
						{
							FVector Position;
							FRotator Orientation;
							float WorldToMeters = GetWorld() ? GetWorld()->GetWorldSettings()->WorldToMeters : 100.0f;
							if (OtherController->GripPollControllerState(Position, Orientation, WorldToMeters))
							{
								frontLoc = OtherController->CalcControllerComponentToWorld(Orientation, Position).GetLocation() - BasePoint;
								///*curLocation*/ frontLoc = OtherController->CalcNewComponentToWorld(FTransform(Orientation, Position)).GetLocation() - BasePoint;
								bPulledControllerLoc = true;
							}
						}
					}
				}

				if (!bPulledControllerLoc)
					/*curLocation*/ frontLoc = Grip.SecondaryGripInfo.SecondaryAttachment->GetComponentLocation() - BasePoint;

				frontLocOrig = (/*WorldTransform*/SecondaryTransform.TransformPosition(Grip.SecondaryGripInfo.SecondaryRelativeTransform.GetLocation())) - BasePoint;

				// Apply any smoothing settings and lerping in / constant lerping
				Default_ApplySmoothingAndLerp(Grip, frontLoc, frontLocOrig, DeltaTime);

				Grip.SecondaryGripInfo.LastRelativeLocation = frontLoc;
			}

			// Get any scaling addition from a scaling secondary grip type
			FVector Scaler = FVector(1.0f);
			Default_GetAnyScaling(Scaler, Grip, frontLoc, frontLocOrig, SecondaryType, SecondaryTransform);

			Grip.SecondaryGripInfo.SecondaryGripDistance = FVector::Dist(frontLocOrig, frontLoc);

			if (Grip.AdvancedGripSettings.SecondaryGripSettings.bUseSecondaryGripSettings && Grip.AdvancedGripSettings.SecondaryGripSettings.bUseSecondaryGripDistanceInfluence)
			{
				float rotScaler = 1.0f - FMath::Clamp((Grip.SecondaryGripInfo.SecondaryGripDistance - Grip.AdvancedGripSettings.SecondaryGripSettings.GripInfluenceDeadZone) / FMath::Max(Grip.AdvancedGripSettings.SecondaryGripSettings.GripInfluenceDistanceToZero, 1.0f), 0.0f, 1.0f);
				frontLoc = FMath::Lerp(frontLocOrig, frontLoc, rotScaler);
			}

			// Skip rot val for scaling only
			if (SecondaryType != ESecondaryGripType::SG_ScalingOnly)
			{
				// Get the rotation difference from the initial second grip
				FQuat rotVal = FQuat::FindBetweenVectors(frontLocOrig, frontLoc);

				// Rebase the world transform to the pivot point, add the rotation, remove the pivot point rebase
				WorldTransform = WorldTransform * WorldToPivot * FTransform(rotVal, FVector::ZeroVector, Scaler) * PivotToWorld;
			}
			else
			{
				// Rebase the world transform to the pivot point, add the scaler, remove the pivot point rebase
				WorldTransform = WorldTransform * WorldToPivot * FTransform(FQuat::Identity, FVector::ZeroVector, Scaler) * PivotToWorld;
			}
		}
	}
	return true;
}

void UGS_GunTools::ResetRecoil()
{
	BackEndRecoilStorage = FTransform::Identity;
	BackEndRecoilTarget = FTransform::Identity;
}

void UGS_GunTools::AddRecoilInstance(const FTransform & RecoilAddition)
{
	if (!bHasRecoil)
		return;

	BackEndRecoilTarget += RecoilAddition;
	// Clamp to max recoil, and + is wrong need to combine.


	FVector CurVec = BackEndRecoilTarget.GetTranslation();

	// Identity on min value is technically wrong, what if they want to recoil in the opposing direction?
	CurVec.X = FMath::Clamp(CurVec.X, FMath::Min(0.f, MaxRecoilTranslation.X), FMath::Max(MaxRecoilTranslation.X, 0.f));
	CurVec.Y = FMath::Clamp(CurVec.Y, FMath::Min(0.f, MaxRecoilTranslation.Y), FMath::Max(MaxRecoilTranslation.Y, 0.f));
	CurVec.Z = FMath::Clamp(CurVec.Z, FMath::Min(0.f, MaxRecoilTranslation.Z), FMath::Max(MaxRecoilTranslation.Z, 0.f));
	BackEndRecoilTarget.SetTranslation(CurVec);

	FVector CurScale = BackEndRecoilTarget.GetScale3D();

	// Identity on min value is technically wrong, what if they want to recoil in the opposing direction?
	CurScale.X = FMath::Clamp(CurScale.X, FMath::Min(0.f, MaxRecoilScale.X), FMath::Max(MaxRecoilScale.X, 0.f));
	CurScale.Y = FMath::Clamp(CurScale.Y, FMath::Min(0.f, MaxRecoilScale.Y), FMath::Max(MaxRecoilScale.Y, 0.f));
	CurScale.Z = FMath::Clamp(CurScale.Z, FMath::Min(0.f, MaxRecoilScale.Z), FMath::Max(MaxRecoilScale.Z, 0.f));
	BackEndRecoilTarget.SetScale3D(CurScale);

	FRotator curRot = BackEndRecoilTarget.Rotator();
	curRot.Pitch = FMath::Clamp(curRot.Pitch, FMath::Min(0.f, MaxRecoilRotation.Y), FMath::Max(MaxRecoilRotation.Y, 0.f));
	curRot.Yaw = FMath::Clamp(curRot.Yaw, FMath::Min(0.f, MaxRecoilRotation.Z), FMath::Max(MaxRecoilRotation.Z, 0.f));
	curRot.Roll = FMath::Clamp(curRot.Roll, FMath::Min(0.f, MaxRecoilRotation.X), FMath::Max(MaxRecoilRotation.X, 0.f));

	BackEndRecoilTarget.SetRotation(curRot.Quaternion());
}