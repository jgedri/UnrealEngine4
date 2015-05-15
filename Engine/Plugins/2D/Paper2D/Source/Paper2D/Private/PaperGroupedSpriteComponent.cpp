// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "Paper2DPrivatePCH.h"
#include "PaperSpriteSceneProxy.h"
#include "PaperSpriteComponent.h"

#include "PaperGroupedSpriteComponent.h"
#include "GroupedSpriteSceneProxy.h"

#include "PaperSprite.h"
#include "PhysicsPublic.h"

#include "MessageLog.h"
#include "UObjectToken.h"

#define LOCTEXT_NAMESPACE "Paper2D"

//////////////////////////////////////////////////////////////////////////
// UPaperGroupedSpriteComponent

UPaperGroupedSpriteComponent::UPaperGroupedSpriteComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);

	Mobility = EComponentMobility::Movable;
	BodyInstance.bSimulatePhysics = false;
}

int32 UPaperGroupedSpriteComponent::AddInstance(const FTransform& LocalTransform, UPaperSprite* Sprite, FColor Color)
{
	const int32 NewInstanceIndex = PerInstanceSpriteData.Num();

	FSpriteInstanceData& NewInstanceData = *new (PerInstanceSpriteData) FSpriteInstanceData();
	SetupNewInstanceData(NewInstanceData, NewInstanceIndex, LocalTransform, Sprite, Color);

	MarkRenderStateDirty();

	UNavigationSystem::UpdateNavOctree(this);

	return NewInstanceIndex;
}

int32 UPaperGroupedSpriteComponent::AddInstanceWorldSpace(const FTransform& WorldTransform, UPaperSprite* Sprite, FColor Color)
{
	// Transform from world space to local space
	const FTransform RelativeTM = WorldTransform.GetRelativeTransform(ComponentToWorld);
	return AddInstance(RelativeTM, Sprite, Color);
}

bool UPaperGroupedSpriteComponent::GetInstanceTransform(int32 InstanceIndex, FTransform& OutInstanceTransform, bool bWorldSpace) const
{
	if (!PerInstanceSpriteData.IsValidIndex(InstanceIndex))
	{
		return false;
	}

	const FSpriteInstanceData& InstanceData = PerInstanceSpriteData[InstanceIndex];

	OutInstanceTransform = FTransform(InstanceData.Transform);
	if (bWorldSpace)
	{
		OutInstanceTransform = OutInstanceTransform * ComponentToWorld;
	}

	return true;
}

bool UPaperGroupedSpriteComponent::UpdateInstanceTransform(int32 InstanceIndex, const FTransform& NewInstanceTransform, bool bWorldSpace, bool bMarkRenderStateDirty)
{
	if (!PerInstanceSpriteData.IsValidIndex(InstanceIndex))
	{
		return false;
	}

	// Request navigation update
	UNavigationSystem::UpdateNavOctree(this);

	FSpriteInstanceData& InstanceData = PerInstanceSpriteData[InstanceIndex];

	// Render data uses local transform of the instance
	FTransform LocalTransform = bWorldSpace ? NewInstanceTransform.GetRelativeTransform(ComponentToWorld) : NewInstanceTransform;
	InstanceData.Transform = LocalTransform.ToMatrixWithScale();

	if (bPhysicsStateCreated)
	{
		// Physics uses world transform of the instance
		const FTransform WorldTransform = bWorldSpace ? NewInstanceTransform : (LocalTransform * ComponentToWorld);
		if (FBodyInstance* InstanceBodyInstance = InstanceBodies[InstanceIndex])
		{
			// Update transform.
			InstanceBodyInstance->SetBodyTransform(WorldTransform, false);
			InstanceBodyInstance->UpdateBodyScale(WorldTransform.GetScale3D());
		}
	}

	// Request navigation update
	UNavigationSystem::UpdateNavOctree(this);

	if (bMarkRenderStateDirty)
	{
		MarkRenderStateDirty();
	}

	return true;
}

bool UPaperGroupedSpriteComponent::RemoveInstance(int32 InstanceIndex)
{
	if (!PerInstanceSpriteData.IsValidIndex(InstanceIndex))
	{
		return false;
	}

	// Request navigation update
	UNavigationSystem::UpdateNavOctree(this);

	// remove instance
	PerInstanceSpriteData.RemoveAt(InstanceIndex);

	// update the physics state
	if (bPhysicsStateCreated)
	{
		// TODO: it may be possible to instead just update the BodyInstanceIndex for all bodies after the removed instance. 
		ClearAllInstanceBodies();
		CreateAllInstanceBodies();
	}

	// Indicate we need to update render state to reflect changes
	MarkRenderStateDirty();

	return true;
}

void UPaperGroupedSpriteComponent::ClearInstances()
{
	// Clear all the per-instance data
	PerInstanceSpriteData.Empty();

	// Release any physics representations
	ClearAllInstanceBodies();

	// Indicate we need to update render state to reflect changes
	MarkRenderStateDirty();

	UNavigationSystem::UpdateNavOctree(this);
}

int32 UPaperGroupedSpriteComponent::GetInstanceCount() const
{
	return PerInstanceSpriteData.Num();
}

bool UPaperGroupedSpriteComponent::ShouldCreatePhysicsState() const
{
	return IsRegistered() && (bAlwaysCreatePhysicsState || IsCollisionEnabled());
}

void UPaperGroupedSpriteComponent::CreatePhysicsState()
{
	check(InstanceBodies.Num() == 0);

	// Create all the bodies.
	CreateAllInstanceBodies();

	USceneComponent::CreatePhysicsState();
}

void UPaperGroupedSpriteComponent::DestroyPhysicsState()
{
	USceneComponent::DestroyPhysicsState();

	// Release all physics representations
	ClearAllInstanceBodies();
}

#if WITH_EDITOR
void UPaperGroupedSpriteComponent::CheckForErrors()
{
	AActor* Owner = GetOwner();

	bool bAnyInstancesWithNoSprites = false;
	for (const FSpriteInstanceData& InstanceData : PerInstanceSpriteData)
	{
		if (InstanceData.SourceSprite == nullptr)
		{
			bAnyInstancesWithNoSprites = true;
			break;
		}
	}

	if (bAnyInstancesWithNoSprites)
	{
		FMessageLog("MapCheck").Warning()
			->AddToken(FUObjectToken::Create(Owner))
			->AddToken(FTextToken::Create(LOCTEXT("MapCheck_Message_InstanceSpriteComponent_MissingSprite", "One or more instances have no sprite asset set!")));
	}

	Super::CheckForErrors();
}
#endif

FPrimitiveSceneProxy* UPaperGroupedSpriteComponent::CreateSceneProxy()
{
	if (PerInstanceSpriteData.Num() > 0)
	{
		return ::new FGroupedSpriteSceneProxy(this);
	}
	else
	{
		return nullptr;
	}
}

bool UPaperGroupedSpriteComponent::CanEditSimulatePhysics()
{
	// Simulating for instanced sprite components is never allowed
	return false;
}

FBoxSphereBounds UPaperGroupedSpriteComponent::CalcBounds(const FTransform& BoundTransform) const
{
	bool bHadAnyBounds = false;
	FBoxSphereBounds NewBounds(ForceInit);

	if (PerInstanceSpriteData.Num() > 0)
	{
		const FMatrix BoundTransformMatrix = BoundTransform.ToMatrixWithScale();

		for (const FSpriteInstanceData& InstanceData : PerInstanceSpriteData)
		{
			if (InstanceData.SourceSprite != nullptr)
			{
				const FBoxSphereBounds RenderBounds = InstanceData.SourceSprite->GetRenderBounds();
				const FBoxSphereBounds InstanceBounds = RenderBounds.TransformBy(InstanceData.Transform * BoundTransformMatrix);

				if (bHadAnyBounds)
				{
					NewBounds = NewBounds + InstanceBounds;
				}
				else
				{
					NewBounds = InstanceBounds;
					bHadAnyBounds = true;
				}
			}
		}
	}

	return bHadAnyBounds ? NewBounds : FBoxSphereBounds(BoundTransform.GetLocation(), FVector::ZeroVector, 0.f);
}

#if WITH_EDITOR
void UPaperGroupedSpriteComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	// Rebuild the material array
	RebuildMaterialList();

	// Rebuild all instances
	//@TODO: Should try and avoid this except when absolutely necessary
	RebuildInstances();

	Super::PostEditChangeProperty(PropertyChangedEvent);
}

void UPaperGroupedSpriteComponent::PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedEvent)
{
	if ((PropertyChangedEvent.Property != nullptr) && (PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UPaperGroupedSpriteComponent, PerInstanceSpriteData)))
	{
		if (PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayAdd)
		{
			const int32 AddedAtIndex = PropertyChangedEvent.GetArrayIndex(PropertyChangedEvent.Property->GetFName().ToString());
			check(AddedAtIndex != INDEX_NONE);
			SetupNewInstanceData(PerInstanceSpriteData[AddedAtIndex], AddedAtIndex, FTransform::Identity, nullptr, FColor::White); //@TODO: Need to pull the sprite from somewhere
		}

		MarkRenderStateDirty();
	}

	Super::PostEditChangeChainProperty(PropertyChangedEvent);
}

void UPaperGroupedSpriteComponent::PostEditUndo()
{
	Super::PostEditUndo();

	UNavigationSystem::UpdateNavOctree(this);
}
#endif

void UPaperGroupedSpriteComponent::CreateAllInstanceBodies()
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_UPaperGroupedSpriteComponent_CreateAllInstanceBodies);

	FPhysScene* PhysScene = GetWorld()->GetPhysicsScene();

	const int32 NumBodies = PerInstanceSpriteData.Num();
	check(InstanceBodies.Num() == 0);
	InstanceBodies.SetNumUninitialized(NumBodies);

	TArray<FTransform> Transforms;
	Transforms.Reserve(NumBodies);

	for (int32 InstanceIndex = 0; InstanceIndex < NumBodies; ++InstanceIndex)
	{
		const FSpriteInstanceData& InstanceData = PerInstanceSpriteData[InstanceIndex];
		InstanceBodies[InstanceIndex] = InitInstanceBody(InstanceIndex, InstanceData, PhysScene);
	}
}

void UPaperGroupedSpriteComponent::ClearAllInstanceBodies()
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_UPaperGroupedSpriteComponent_ClearAllInstanceBodies);

	for (FBodyInstance* OldBodyInstance : InstanceBodies)
	{
		if (OldBodyInstance != nullptr)
		{
			OldBodyInstance->TermBody();
			delete OldBodyInstance;
		}
	}

	InstanceBodies.Empty();
}

void UPaperGroupedSpriteComponent::SetupNewInstanceData(FSpriteInstanceData& InOutNewInstanceData, int32 InInstanceIndex, const FTransform& InInstanceTransform, UPaperSprite* InSprite, const FColor& InColor)
{
	InOutNewInstanceData.Transform = InInstanceTransform.ToMatrixWithScale();
	InOutNewInstanceData.SourceSprite = InSprite;
	InOutNewInstanceData.VertexColor = InColor;
	InOutNewInstanceData.MaterialIndex = UpdateMaterialList(InSprite);

	if (bPhysicsStateCreated && (InSprite != nullptr) && (InSprite->BodySetup != nullptr))
	{
		FPhysScene* PhysScene = GetWorld()->GetPhysicsScene();
		FBodyInstance* NewBodyInstance = InitInstanceBody(InInstanceIndex, InOutNewInstanceData, PhysScene);

		InstanceBodies.Insert(NewBodyInstance, InInstanceIndex);
	}
}

FBodyInstance* UPaperGroupedSpriteComponent::InitInstanceBody(int32 InstanceIndex, const FSpriteInstanceData& InstanceData, FPhysScene* PhysScene)
{
	FBodyInstance* NewBodyInstance = nullptr;

	if (InstanceData.SourceSprite != nullptr)
	{
		if (UBodySetup* BodySetup = InstanceData.SourceSprite->BodySetup)
		{
			NewBodyInstance = new FBodyInstance();

			NewBodyInstance->CopyBodyInstancePropertiesFrom(&BodyInstance);
			NewBodyInstance->InstanceBodyIndex = InstanceIndex; // Set body index 
			NewBodyInstance->bAutoWeld = false;

			// make sure we never enable bSimulatePhysics for ISMComps
			NewBodyInstance->bSimulatePhysics = false;

			const FTransform InstanceTransform(FTransform(InstanceData.Transform) * ComponentToWorld);
			NewBodyInstance->InitBody(BodySetup, InstanceTransform, this, PhysScene);
		}
	}

	return NewBodyInstance;
}

void UPaperGroupedSpriteComponent::RebuildInstances()
{
	// update the physics state
	if (bPhysicsStateCreated)
	{
		// TODO: it may be possible to instead just update the BodyInstanceIndex for all bodies after the removed instance. 
		ClearAllInstanceBodies();
		CreateAllInstanceBodies();
	}

	// Indicate we need to update render state to reflect changes
	MarkRenderStateDirty();
}

void UPaperGroupedSpriteComponent::RebuildMaterialList()
{
	InstanceMaterials.Reset();

	//@TODO: Need to rebuild the OverrideMaterials array and update the per-sprite material indices as part of this process

	for (const FSpriteInstanceData& InstanceData : PerInstanceSpriteData)
	{
		UpdateMaterialList(InstanceData.SourceSprite);
	}
}

int32 UPaperGroupedSpriteComponent::UpdateMaterialList(UPaperSprite* Sprite)
{
	int32 Result = INDEX_NONE;

	if (Sprite != nullptr)
	{
		if (UMaterialInterface* SpriteMaterial = Sprite->GetMaterial(0))
		{
			Result = InstanceMaterials.AddUnique(SpriteMaterial);
		}
	}

	return Result;
}

void UPaperGroupedSpriteComponent::GetUsedTextures(TArray<UTexture*>& OutTextures, EMaterialQualityLevel::Type QualityLevel)
{
	// Get the textures referenced by any sprite instances
	for (const FSpriteInstanceData& InstanceData : PerInstanceSpriteData)
	{
		if (InstanceData.SourceSprite != nullptr)
		{
			if (UTexture* BakedTexture = InstanceData.SourceSprite->GetBakedTexture())
			{
				OutTextures.AddUnique(BakedTexture);
			}

			FAdditionalSpriteTextureArray AdditionalTextureList;
			InstanceData.SourceSprite->GetBakedAdditionalSourceTextures(/*out*/ AdditionalTextureList);
			for (UTexture* AdditionalTexture : AdditionalTextureList)
			{
				if (AdditionalTexture != nullptr)
				{
					OutTextures.AddUnique(AdditionalTexture);
				}
			}
		}
	}

	// Get any textures referenced by our materials
	Super::GetUsedTextures(OutTextures, QualityLevel);
}

UMaterialInterface* UPaperGroupedSpriteComponent::GetMaterial(int32 MaterialIndex) const
{
	if (OverrideMaterials.IsValidIndex(MaterialIndex))
	{
		if (UMaterialInterface* OverrideMaterial = OverrideMaterials[MaterialIndex])
		{
			return OverrideMaterial;
		}
	}

	if (InstanceMaterials.IsValidIndex(MaterialIndex))
	{
		return InstanceMaterials[MaterialIndex];
	}

	return nullptr;
}

int32 UPaperGroupedSpriteComponent::GetNumMaterials() const
{
	return FMath::Max<int32>(OverrideMaterials.Num(), FMath::Max<int32>(InstanceMaterials.Num(), 1));
}

bool UPaperGroupedSpriteComponent::ContainsSprite(UPaperSprite* SpriteAsset) const
{
	for (const FSpriteInstanceData& InstanceData : PerInstanceSpriteData)
	{
		if (InstanceData.SourceSprite == SpriteAsset)
		{
			return true;
		}
	}

	return false;
}

//////////////////////////////////////////////////////////////////////////

#undef LOCTEXT_NAMESPACE