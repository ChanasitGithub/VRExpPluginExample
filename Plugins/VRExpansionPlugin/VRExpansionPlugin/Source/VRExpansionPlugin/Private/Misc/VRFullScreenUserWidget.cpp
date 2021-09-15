// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/VRFullScreenUserWidget.h"

#include "Components/PostProcessComponent.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/UserInterfaceSettings.h"
#include "GameFramework/WorldSettings.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "UObject/ConstructorHelpers.h"
#include "HAL/PlatformApplicationMisc.h"

#include "Framework/Application/SlateApplication.h"
#include "Input/HittestGrid.h"
#include "Layout/Visibility.h"
#include "Slate/SceneViewport.h"
#include "Slate/WidgetRenderer.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Widgets/SViewport.h"

#if WITH_EDITOR
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "SLevelViewport.h"
#endif

#define LOCTEXT_NAMESPACE "VRFullScreenUserWidget"

/////////////////////////////////////////////////////
// Internal helper
namespace
{
	const FName NAME_LevelEditorName = "LevelEditor";
	const FName NAME_SlateUI = "SlateUI";
	const FName NAME_TintColorAndOpacity = "TintColorAndOpacity";
	const FName NAME_OpacityFromTexture = "OpacityFromTexture";

	EVisibility ConvertWindowVisibilityToVisibility(EWindowVisibility visibility)
	{
		switch (visibility)
		{
		case EWindowVisibility::Visible:
			return EVisibility::Visible;
		case EWindowVisibility::SelfHitTestInvisible:
			return EVisibility::SelfHitTestInvisible;
		default:
			checkNoEntry();
			return EVisibility::SelfHitTestInvisible;
		}
	}
}


/////////////////////////////////////////////////////
// FVRWidgetPostProcessHitTester
class FVRWidgetPostProcessHitTester : public ICustomHitTestPath
{
public:
	FVRWidgetPostProcessHitTester(UWorld* InWorld, TSharedPtr<SVirtualWindow> InSlateWindow)
		: World(InWorld)
		, SlateWindow(InSlateWindow)
		, WidgetDrawSize(FIntPoint::ZeroValue)
		, LastLocalHitLocation(FVector2D::ZeroVector)
	{}

	virtual TArray<FWidgetAndPointer> GetBubblePathAndVirtualCursors(const FGeometry& InGeometry, FVector2D DesktopSpaceCoordinate, bool bIgnoreEnabledStatus) const override
	{
		// Get the list of widget at the requested location.
		TArray<FWidgetAndPointer> ArrangedWidgets;
		if (TSharedPtr<SVirtualWindow> SlateWindowPin = SlateWindow.Pin())
		{
			FVector2D LocalMouseCoordinate = InGeometry.AbsoluteToLocal(DesktopSpaceCoordinate);
			float CursorRadius = 0.f;
			ArrangedWidgets = SlateWindowPin->GetHittestGrid().GetBubblePath(LocalMouseCoordinate, CursorRadius, bIgnoreEnabledStatus);

			TSharedRef<FVirtualPointerPosition> VirtualMouseCoordinate = MakeShared<FVirtualPointerPosition>();
			VirtualMouseCoordinate->CurrentCursorPosition = LocalMouseCoordinate;
			VirtualMouseCoordinate->LastCursorPosition = LastLocalHitLocation;

			LastLocalHitLocation = LocalMouseCoordinate;

			for (FWidgetAndPointer& ArrangedWidget : ArrangedWidgets)
			{
				ArrangedWidget.PointerPosition = VirtualMouseCoordinate;
			}
		}

		return ArrangedWidgets;
	}

	virtual void ArrangeCustomHitTestChildren(FArrangedChildren& ArrangedChildren) const override
	{
		// Add the displayed slate to the list of widgets.
		if (TSharedPtr<SVirtualWindow> SlateWindowPin = SlateWindow.Pin())
		{
			FGeometry WidgetGeom;
			ArrangedChildren.AddWidget(FArrangedWidget(SlateWindowPin.ToSharedRef(), WidgetGeom.MakeChild(WidgetDrawSize, FSlateLayoutTransform())));
		}
	}

	virtual TSharedPtr<struct FVirtualPointerPosition> TranslateMouseCoordinateForCustomHitTestChild(const TSharedRef<SWidget>& ChildWidget, const FGeometry& ViewportGeometry, const FVector2D& ScreenSpaceMouseCoordinate, const FVector2D& LastScreenSpaceMouseCoordinate) const override
	{
		return nullptr;
	}

	void SetWidgetDrawSize(FIntPoint NewWidgetDrawSize)
	{
		WidgetDrawSize = NewWidgetDrawSize;
	}

private:
	TWeakObjectPtr<UWorld> World;
	TWeakPtr<SVirtualWindow> SlateWindow;
	FIntPoint WidgetDrawSize;
	mutable FVector2D LastLocalHitLocation;
};

/////////////////////////////////////////////////////
// FVRFullScreenUserWidget_Viewport
FVRFullScreenUserWidget_Viewport::FVRFullScreenUserWidget_Viewport()
	: bAddedToGameViewport(false)
{
}

bool FVRFullScreenUserWidget_Viewport::Display(UWorld* World, UUserWidget* Widget, float InDPIScale)
{
	TSharedPtr<SConstraintCanvas> FullScreenWidgetPinned = FullScreenCanvasWidget.Pin();
	if (Widget == nullptr || World == nullptr || FullScreenWidgetPinned.IsValid())
	{
		return false;
	}

	UGameViewportClient* ViewportClient = nullptr;
#if WITH_EDITOR
	TSharedPtr<SLevelViewport> ActiveLevelViewport;
#endif

	bool bResult = false;
	if (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE)
	{
		ViewportClient = World->GetGameViewport();
		bResult = ViewportClient != nullptr;
	}
#if WITH_EDITOR
	else if (FModuleManager::Get().IsModuleLoaded(NAME_LevelEditorName))
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(NAME_LevelEditorName);
		if (TargetViewport.IsValid())
		{
			ActiveLevelViewport = TargetViewport.Pin();
		}
		else
		{
			ActiveLevelViewport = LevelEditorModule.GetFirstActiveLevelViewport();
		}
		bResult = ActiveLevelViewport.IsValid();
	}
#endif

	if (bResult)
	{
		TSharedRef<SConstraintCanvas> FullScreenCanvas = SNew(SConstraintCanvas);
		FullScreenCanvasWidget = FullScreenCanvas;

		FullScreenCanvas->AddSlot()
			.Offset(FMargin(0, 0, 0, 0))
			.Anchors(FAnchors(0, 0, 1, 1))
			.Alignment(FVector2D(0, 0))
			[
				SNew(SDPIScaler)
				.DPIScale(InDPIScale)
				[
					Widget->TakeWidget()
				]
			];

		if (ViewportClient)
		{
			ViewportClient->AddViewportWidgetContent(FullScreenCanvas);
		}
#if WITH_EDITOR
		else
		{
			check(ActiveLevelViewport.IsValid());
			ActiveLevelViewport->AddOverlayWidget(FullScreenCanvas);
			OverlayWidgetLevelViewport = ActiveLevelViewport;
		}
#endif
	}

	return bResult;
}

void FVRFullScreenUserWidget_Viewport::Hide(UWorld* World)
{
	TSharedPtr<SConstraintCanvas> FullScreenWidgetPinned = FullScreenCanvasWidget.Pin();
	if (FullScreenWidgetPinned.IsValid())
	{
		// Remove from Viewport and Fullscreen, in case the settings changed before we had the chance to hide.
		UGameViewportClient* ViewportClient = World ? World->GetGameViewport() : nullptr;
		if (ViewportClient)
		{
			ViewportClient->RemoveViewportWidgetContent(FullScreenWidgetPinned.ToSharedRef());
		}

#if WITH_EDITOR
		TSharedPtr<SLevelViewport> OverlayWidgetLevelViewportPinned = OverlayWidgetLevelViewport.Pin();
		if (OverlayWidgetLevelViewportPinned)
		{
			OverlayWidgetLevelViewportPinned->RemoveOverlayWidget(FullScreenWidgetPinned.ToSharedRef());
		}
		OverlayWidgetLevelViewport.Reset();
#endif

		FullScreenCanvasWidget.Reset();
	}
}

void FVRFullScreenUserWidget_Viewport::Tick(UWorld* World, float DeltaSeconds)
{

}

/////////////////////////////////////////////////////
// FVRFullScreenUserWidget_PostProcess

FVRFullScreenUserWidget_PostProcess::FVRFullScreenUserWidget_PostProcess()
	: PostProcessMaterial(nullptr)
	, PostProcessTintColorAndOpacity(FLinearColor::White)
	, PostProcessOpacityFromTexture(1.0f)
	, bWidgetDrawSize(false)
	, WidgetDrawSize(FIntPoint(640, 360))
	, bWindowFocusable(true)
	, WindowVisibility(EWindowVisibility::SelfHitTestInvisible)
	, bReceiveHardwareInput(false)
	, RenderTargetBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 1.0f))
	, RenderTargetBlendMode(EWidgetBlendMode::Masked)
	, WidgetRenderTarget(nullptr)
	, PostProcessComponent(nullptr)
	, PostProcessMaterialInstance(nullptr)
	, WidgetRenderer(nullptr)
	, CurrentWidgetDrawSize(FIntPoint::ZeroValue)
{
}

bool FVRFullScreenUserWidget_PostProcess::Display(UWorld* World, UUserWidget* Widget, bool bInRenderToTextureOnly, float InDPIScale)
{
	bRenderToTextureOnly = bInRenderToTextureOnly;

	bool bOk = CreateRenderer(World, Widget, InDPIScale);
	if (!bRenderToTextureOnly)
	{
		bOk &= CreatePostProcessComponent(World);
	}

	return bOk;
}

void FVRFullScreenUserWidget_PostProcess::Hide(UWorld* World)
{
	if (!bRenderToTextureOnly)
	{
		ReleasePostProcessComponent();
	}

	ReleaseRenderer();
}

void FVRFullScreenUserWidget_PostProcess::Tick(UWorld* World, float DeltaSeconds)
{
	TickRenderer(World, DeltaSeconds);
}

TSharedPtr<SVirtualWindow> FVRFullScreenUserWidget_PostProcess::GetSlateWindow() const
{
	return SlateWindow;
}

bool FVRFullScreenUserWidget_PostProcess::CreatePostProcessComponent(UWorld* World)
{
	ReleasePostProcessComponent();
	if (World && PostProcessMaterial)
	{
		AWorldSettings* WorldSetting = World->GetWorldSettings();
		PostProcessComponent = NewObject<UPostProcessComponent>(WorldSetting, NAME_None, RF_Transient);
		PostProcessComponent->bEnabled = true;
		PostProcessComponent->bUnbound = true;
		PostProcessComponent->RegisterComponent();

		PostProcessMaterialInstance = UMaterialInstanceDynamic::Create(PostProcessMaterial, World);

		// set the parameter immediately
		PostProcessMaterialInstance->SetTextureParameterValue(NAME_SlateUI, WidgetRenderTarget);
		PostProcessMaterialInstance->SetVectorParameterValue(NAME_TintColorAndOpacity, PostProcessTintColorAndOpacity);
		PostProcessMaterialInstance->SetScalarParameterValue(NAME_OpacityFromTexture, PostProcessOpacityFromTexture);

		PostProcessComponent->Settings.WeightedBlendables.Array.SetNumZeroed(1);
		PostProcessComponent->Settings.WeightedBlendables.Array[0].Weight = 1.f;
		PostProcessComponent->Settings.WeightedBlendables.Array[0].Object = PostProcessMaterialInstance;
	}

	return PostProcessComponent && PostProcessMaterialInstance;
}

void FVRFullScreenUserWidget_PostProcess::ReleasePostProcessComponent()
{
	if (PostProcessComponent)
	{
		PostProcessComponent->UnregisterComponent();
	}
	PostProcessComponent = nullptr;
	PostProcessMaterialInstance = nullptr;
}

bool FVRFullScreenUserWidget_PostProcess::CreateRenderer(UWorld* World, UUserWidget* Widget, float InDPIScale)
{
	ReleaseRenderer();

	if (World && Widget)
	{
		const FIntPoint CalculatedWidgetSize = CalculateWidgetDrawSize(World);
		if (IsTextureSizeValid(CalculatedWidgetSize))
		{
			CurrentWidgetDrawSize = CalculatedWidgetSize;

			const bool bApplyGammaCorrection = true;
			WidgetRenderer = new FWidgetRenderer(bApplyGammaCorrection);
			WidgetRenderer->SetIsPrepassNeeded(true);

			SlateWindow = SNew(SVirtualWindow).Size(CurrentWidgetDrawSize);
			SlateWindow->SetIsFocusable(bWindowFocusable);
			SlateWindow->SetVisibility(ConvertWindowVisibilityToVisibility(WindowVisibility));
			SlateWindow->SetContent(SNew(SDPIScaler).DPIScale(InDPIScale)
				[
					Widget->TakeWidget()
				]
			);

			RegisterHitTesterWithViewport(World);

			if (!Widget->IsDesignTime() && World->IsGameWorld())
			{
				UGameInstance* GameInstance = World->GetGameInstance();
				UGameViewportClient* GameViewportClient = GameInstance ? GameInstance->GetGameViewportClient() : nullptr;
				if (GameViewportClient)
				{
					SlateWindow->AssignParentWidget(GameViewportClient->GetGameViewportWidget());
				}
			}

			FLinearColor ActualBackgroundColor = RenderTargetBackgroundColor;
			switch (RenderTargetBlendMode)
			{
			case EWidgetBlendMode::Opaque:
				ActualBackgroundColor.A = 1.0f;
				break;
			case EWidgetBlendMode::Masked:
				ActualBackgroundColor.A = 0.0f;
				break;
			}

			AWorldSettings* WorldSetting = World->GetWorldSettings();
			WidgetRenderTarget = NewObject<UTextureRenderTarget2D>(WorldSetting, NAME_None, RF_Transient);
			WidgetRenderTarget->ClearColor = ActualBackgroundColor;
			WidgetRenderTarget->InitCustomFormat(CurrentWidgetDrawSize.X, CurrentWidgetDrawSize.Y, PF_B8G8R8A8, false);
			WidgetRenderTarget->UpdateResourceImmediate();

			if (!bRenderToTextureOnly && PostProcessMaterialInstance)
			{
				PostProcessMaterialInstance->SetTextureParameterValue(NAME_SlateUI, WidgetRenderTarget);
			}
		}
	}

	return WidgetRenderer && WidgetRenderTarget;
}

void FVRFullScreenUserWidget_PostProcess::ReleaseRenderer()
{
	if (WidgetRenderer)
	{
		BeginCleanup(WidgetRenderer);
		WidgetRenderer = nullptr;
	}
	UnRegisterHitTesterWithViewport();

	SlateWindow.Reset();
	WidgetRenderTarget = nullptr;
	CurrentWidgetDrawSize = FIntPoint::ZeroValue;
}

void FVRFullScreenUserWidget_PostProcess::TickRenderer(UWorld* World, float DeltaSeconds)
{
	check(World);
	if (WidgetRenderTarget)
	{
		const float DrawScale = 1.0f;

		const FIntPoint NewCalculatedWidgetSize = CalculateWidgetDrawSize(World);
		if (NewCalculatedWidgetSize != CurrentWidgetDrawSize)
		{
			if (IsTextureSizeValid(NewCalculatedWidgetSize))
			{
				CurrentWidgetDrawSize = NewCalculatedWidgetSize;
				WidgetRenderTarget->InitCustomFormat(CurrentWidgetDrawSize.X, CurrentWidgetDrawSize.Y, PF_B8G8R8A8, false);
				WidgetRenderTarget->UpdateResourceImmediate();
				SlateWindow->Resize(CurrentWidgetDrawSize);
				if (CustomHitTestPath)
				{
					CustomHitTestPath->SetWidgetDrawSize(CurrentWidgetDrawSize);
				}
			}
			else
			{
				Hide(World);
			}
		}

		if (WidgetRenderer)
		{
			WidgetRenderer->DrawWindow(
				WidgetRenderTarget,
				SlateWindow->GetHittestGrid(),
				SlateWindow.ToSharedRef(),
				DrawScale,
				CurrentWidgetDrawSize,
				DeltaSeconds);
		}
	}
}

FIntPoint FVRFullScreenUserWidget_PostProcess::CalculateWidgetDrawSize(UWorld* World)
{
	if (bWidgetDrawSize)
	{
		return WidgetDrawSize;
	}

	if (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE)
	{
		if (UGameViewportClient* ViewportClient = World->GetGameViewport())
		{
			// The viewport maybe resizing or not yet initialized.
			//See TickRenderer(), it will be resize on the next tick to the proper size.
			//We initialized all the rendering with an small size.

			const float SmallWidgetSize = 16.f;
			FVector2D OutSize = FVector2D(SmallWidgetSize, SmallWidgetSize);
			ViewportClient->GetViewportSize(OutSize);
			if (OutSize.X < SMALL_NUMBER)
			{
				OutSize = FVector2D(SmallWidgetSize, SmallWidgetSize);
			}
			return OutSize.IntPoint();
		}
	}
#if WITH_EDITOR
	else if (FModuleManager::Get().IsModuleLoaded(NAME_LevelEditorName))
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(NAME_LevelEditorName);
		TSharedPtr<SLevelViewport> ActiveLevelViewport;
		if (TargetViewport.IsValid())
		{
			ActiveLevelViewport = TargetViewport.Pin();
		}
		else
		{
			ActiveLevelViewport = LevelEditorModule.GetFirstActiveLevelViewport();
		}
		if (ActiveLevelViewport.IsValid())
		{
			if (TSharedPtr<FSceneViewport> SharedActiveViewport = ActiveLevelViewport->GetSharedActiveViewport())
			{
				return SharedActiveViewport->GetSize();
			}
		}
	}
#endif
	return FIntPoint::ZeroValue;
}

bool FVRFullScreenUserWidget_PostProcess::IsTextureSizeValid(FIntPoint Size) const
{
	const int32 MaxAllowedDrawSize = GetMax2DTextureDimension();
	return Size.X > 0 && Size.Y > 0 && Size.X <= MaxAllowedDrawSize && Size.Y <= MaxAllowedDrawSize;
}

void FVRFullScreenUserWidget_PostProcess::RegisterHitTesterWithViewport(UWorld* World)
{
	if (!bReceiveHardwareInput && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().RegisterVirtualWindow(SlateWindow.ToSharedRef());
	}

	TSharedPtr<SViewport> EngineViewportWidget;
	if (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE)
	{
		EngineViewportWidget = GEngine->GetGameViewportWidget();
	}
#if WITH_EDITOR
	else if (FModuleManager::Get().IsModuleLoaded(NAME_LevelEditorName))
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(NAME_LevelEditorName);

		TSharedPtr<SLevelViewport> ActiveLevelViewport;
		if (TargetViewport.IsValid())
		{
			ActiveLevelViewport = TargetViewport.Pin();
		}
		else
		{
			ActiveLevelViewport = LevelEditorModule.GetFirstActiveLevelViewport();
		}
		if (ActiveLevelViewport.IsValid())
		{
			EngineViewportWidget = ActiveLevelViewport->GetViewportWidget().Pin();
		}
	}
#endif

	if (EngineViewportWidget && bReceiveHardwareInput)
	{
		if (EngineViewportWidget->GetCustomHitTestPath())
		{
			//UE_LOG(LogVPUtilities, Warning, TEXT("Can't register a hit tester for FullScreenUserWidget. There is already one defined."));
		}
		else
		{
			ViewportWidget = EngineViewportWidget;
			CustomHitTestPath = MakeShared<FVRWidgetPostProcessHitTester>(World, SlateWindow);
			CustomHitTestPath->SetWidgetDrawSize(CurrentWidgetDrawSize);
			EngineViewportWidget->SetCustomHitTestPath(CustomHitTestPath);
		}
	}
}

void FVRFullScreenUserWidget_PostProcess::UnRegisterHitTesterWithViewport()
{
	if (SlateWindow.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterVirtualWindow(SlateWindow.ToSharedRef());
	}

	if (TSharedPtr<SViewport> ViewportWidgetPin = ViewportWidget.Pin())
	{
		if (ViewportWidgetPin->GetCustomHitTestPath() == CustomHitTestPath)
		{
			ViewportWidgetPin->SetCustomHitTestPath(nullptr);
		}
	}

	ViewportWidget.Reset();
	CustomHitTestPath.Reset();
}

/////////////////////////////////////////////////////
// UVRFullScreenUserWidget

UVRFullScreenUserWidget::UVRFullScreenUserWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, CurrentDisplayType(EVRWidgetDisplayType::Inactive)
	, bDisplayRequested(false)
{
	//Material'/VRExpansionPlugin/Materials/VRWidgetPostProcessMaterial.WidgetPostProcessMaterial'
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> PostProcessMaterial_Finder(TEXT("/VRExpansionPlugin/Materials/VRWidgetPostProcessMaterial"));
	PostProcessDisplayType.PostProcessMaterial = PostProcessMaterial_Finder.Object;
}

void UVRFullScreenUserWidget::BeginDestroy()
{
	Hide();
	Super::BeginDestroy();
}

bool UVRFullScreenUserWidget::ShouldDisplay(UWorld* InWorld) const
{
#if UE_SERVER
	return false;
#else
	if (GUsingNullRHI || HasAnyFlags(RF_ArchetypeObject | RF_ClassDefaultObject) || IsRunningDedicatedServer())
	{
		return false;
	}

	return GetDisplayType(InWorld) != EVRWidgetDisplayType::Inactive;
#endif //!UE_SERVER
}

EVRWidgetDisplayType UVRFullScreenUserWidget::GetDisplayType(UWorld* InWorld) const
{
	if (InWorld)
	{
		if (InWorld->WorldType == EWorldType::Game)
		{
			return GameDisplayType;
		}
#if WITH_EDITOR
		else if (InWorld->WorldType == EWorldType::PIE)
		{
			return PIEDisplayType;
		}
		else if (InWorld->WorldType == EWorldType::Editor)
		{
			return EditorDisplayType;
		}
#endif // WITH_EDITOR
	}
	return EVRWidgetDisplayType::Inactive;
}

bool UVRFullScreenUserWidget::IsDisplayed() const
{
	return CurrentDisplayType != EVRWidgetDisplayType::Inactive;
}

bool UVRFullScreenUserWidget::Display(UWorld* InWorld)
{
	bDisplayRequested = true;

	World = InWorld;

	bool bWasAdded = false;
	if (InWorld && WidgetClass && ShouldDisplay(InWorld) && CurrentDisplayType == EVRWidgetDisplayType::Inactive)
	{
		CurrentDisplayType = GetDisplayType(InWorld);

		InitWidget();

		const float DPIScale = GetViewportDPIScale();

		if (CurrentDisplayType == EVRWidgetDisplayType::Viewport)
		{
			bWasAdded = ViewportDisplayType.Display(InWorld, Widget, DPIScale);
		}
		else if ((CurrentDisplayType == EVRWidgetDisplayType::PostProcess) /*|| (CurrentDisplayType == EVRWidgetDisplayType::Composure)*/)
		{
			bWasAdded = PostProcessDisplayType.Display(InWorld, Widget, /*(CurrentDisplayType == EVRWidgetDisplayType::Composure)*/false, DPIScale);
		}

		if (bWasAdded)
		{
			FWorldDelegates::LevelRemovedFromWorld.AddUObject(this, &UVRFullScreenUserWidget::OnLevelRemovedFromWorld);

			// If we are using Composure as our output, then send the WidgetRenderTarget to each one
			/*if (CurrentDisplayType == EVRWidgetDisplayType::Composure)
			{
				static const FString TextureCompClassName("BP_TextureRTCompElement_C");
				static const FName TextureInputPropertyName("TextureRTInput");

				for (ACompositingElement* Layer : PostProcessDisplayType.ComposureLayerTargets)
				{
					if (Layer && (Layer->GetClass()->GetName() == TextureCompClassName))
					{
						FProperty* TextureInputProperty = Layer->GetClass()->FindPropertyByName(TextureInputPropertyName);
						if (TextureInputProperty)
						{
							FObjectProperty* TextureInputObjectProperty = CastField<FObjectProperty>(TextureInputProperty);
							if (TextureInputObjectProperty)
							{
								UTextureRenderTarget2D** DestTextureRT2D = TextureInputProperty->ContainerPtrToValuePtr<UTextureRenderTarget2D*>(Layer);
								if (DestTextureRT2D)
								{
									TextureInputObjectProperty->SetObjectPropertyValue(DestTextureRT2D, PostProcessDisplayType.WidgetRenderTarget);
									Layer->RerunConstructionScripts();
								}
							}
						}
					}
					else if (Layer)
					{
						UE_LOG(LogVPUtilities, Warning, TEXT("VRFullScreenUserWidget - ComposureLayerTarget entry '%s' is not the correct class '%s'"), *Layer->GetName(), *TextureCompClassName);
					}
				}
			}*/
		}
	}

	return bWasAdded;
}

void UVRFullScreenUserWidget::Hide()
{
	bDisplayRequested = false;

	if (CurrentDisplayType != EVRWidgetDisplayType::Inactive)
	{
		ReleaseWidget();
		FWorldDelegates::LevelRemovedFromWorld.RemoveAll(this);

		if (CurrentDisplayType == EVRWidgetDisplayType::Viewport)
		{
			ViewportDisplayType.Hide(World.Get());
		}
		else if ((CurrentDisplayType == EVRWidgetDisplayType::PostProcess) /*|| (CurrentDisplayType == EVRWidgetDisplayType::Composure)*/)
		{
			PostProcessDisplayType.Hide(World.Get());
		}
		CurrentDisplayType = EVRWidgetDisplayType::Inactive;
	}

	World.Reset();
}

void UVRFullScreenUserWidget::Tick(float DeltaSeconds)
{
	if (CurrentDisplayType != EVRWidgetDisplayType::Inactive)
	{
		UWorld* CurrentWorld = World.Get();
		if (CurrentWorld == nullptr)
		{
			Hide();
		}
		else
		{
			if (CurrentDisplayType == EVRWidgetDisplayType::Viewport)
			{
				ViewportDisplayType.Tick(CurrentWorld, DeltaSeconds);
			}
			else if ((CurrentDisplayType == EVRWidgetDisplayType::PostProcess) /*|| (CurrentDisplayType == EVRWidgetDisplayType::Composure)*/)
			{
				PostProcessDisplayType.Tick(CurrentWorld, DeltaSeconds);
			}
		}
	}
}

void UVRFullScreenUserWidget::SetDisplayTypes(EVRWidgetDisplayType InEditorDisplayType, EVRWidgetDisplayType InGameDisplayType, EVRWidgetDisplayType InPIEDisplayType)
{
	EditorDisplayType = InEditorDisplayType;
	GameDisplayType = InGameDisplayType;
	PIEDisplayType = InPIEDisplayType;
}

void UVRFullScreenUserWidget::InitWidget()
{
	// Don't do any work if Slate is not initialized
	if (FSlateApplication::IsInitialized())
	{
		if (WidgetClass && Widget == nullptr)
		{
			check(World.Get());
			Widget = CreateWidget(World.Get(), WidgetClass);
			Widget->SetFlags(RF_Transient);
		}
	}
}

void UVRFullScreenUserWidget::ReleaseWidget()
{
	Widget = nullptr;
}

void UVRFullScreenUserWidget::OnLevelRemovedFromWorld(ULevel* InLevel, UWorld* InWorld)
{
	// If the InLevel is invalid, then the entire world is about to disappear.
	//Hide the widget to clear the memory and reference to the world it may hold.
	if (InLevel == nullptr && InWorld && InWorld == World.Get())
	{
		Hide();
	}
}

FVector2D UVRFullScreenUserWidget::FindSceneViewportSize()
{
	FVector2D OutSize;

	UWorld* CurrentWorld = World.Get();
	if (CurrentWorld && (CurrentWorld->WorldType == EWorldType::Game || CurrentWorld->WorldType == EWorldType::PIE))
	{
		if (UGameViewportClient* ViewportClient = World->GetGameViewport())
		{
			ViewportClient->GetViewportSize(OutSize);
		}
	}
#if WITH_EDITOR
	else if (FModuleManager::Get().IsModuleLoaded(NAME_LevelEditorName))
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>(NAME_LevelEditorName);
		TSharedPtr<SLevelViewport> ActiveLevelViewport;
		if (TargetViewport.IsValid())
		{
			ActiveLevelViewport = TargetViewport.Pin();
		}
		else
		{
			ActiveLevelViewport = LevelEditorModule.GetFirstActiveLevelViewport();
		}
		if (ActiveLevelViewport.IsValid())
		{
			if (TSharedPtr<FSceneViewport> SharedActiveViewport = ActiveLevelViewport->GetSharedActiveViewport())
			{
				OutSize = FVector2D(SharedActiveViewport->GetSize());
			}
		}
	}
#endif

	return OutSize;
}

float UVRFullScreenUserWidget::GetViewportDPIScale()
{
	float UIScale = 1.0f;
	float PlatformScale = FPlatformApplicationMisc::GetDPIScaleFactorAtPoint(10.0f, 10.0f);

	UWorld* CurrentWorld = World.Get();
	if ((CurrentDisplayType == EVRWidgetDisplayType::Viewport) && CurrentWorld && (CurrentWorld->WorldType == EWorldType::Game || CurrentWorld->WorldType == EWorldType::PIE))
	{
		// If we are in Game or PIE in Viewport display mode, the GameLayerManager will scale correctly so just return the Platform Scale
		UIScale = PlatformScale;
	}
	else
	{
		// Otherwise when in Editor mode, the editor automatically scales to the platform size, so we only care about the UI scale
		FIntPoint ViewportSize = FindSceneViewportSize().IntPoint();

		const UUserInterfaceSettings* UserInterfaceSettings = GetDefault<UUserInterfaceSettings>(UUserInterfaceSettings::StaticClass());
		if (UserInterfaceSettings)
		{
			UIScale = UserInterfaceSettings->GetDPIScaleBasedOnSize(ViewportSize);
		}
	}

	return UIScale;
}


#if WITH_EDITOR
void UVRFullScreenUserWidget::SetAllTargetViewports(TWeakPtr<SLevelViewport> InTargetViewport)
{
	TargetViewport = InTargetViewport;
	ViewportDisplayType.TargetViewport = InTargetViewport;
	PostProcessDisplayType.TargetViewport = InTargetViewport;
}

void UVRFullScreenUserWidget::ResetAllTargetViewports()
{
	TargetViewport.Reset();
	ViewportDisplayType.TargetViewport.Reset();
	PostProcessDisplayType.TargetViewport.Reset();
}

void UVRFullScreenUserWidget::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	FProperty* Property = PropertyChangedEvent.MemberProperty;

	if (Property && PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
	{
		static FName NAME_WidgetClass = GET_MEMBER_NAME_CHECKED(UVRFullScreenUserWidget, WidgetClass);
		static FName NAME_EditorDisplayType = GET_MEMBER_NAME_CHECKED(UVRFullScreenUserWidget, EditorDisplayType);
		static FName NAME_PostProcessMaterial = GET_MEMBER_NAME_CHECKED(FVRFullScreenUserWidget_PostProcess, PostProcessMaterial);
		static FName NAME_WidgetDrawSize = GET_MEMBER_NAME_CHECKED(FVRFullScreenUserWidget_PostProcess, WidgetDrawSize);
		static FName NAME_WindowFocusable = GET_MEMBER_NAME_CHECKED(FVRFullScreenUserWidget_PostProcess, bWindowFocusable);
		static FName NAME_WindowVisibility = GET_MEMBER_NAME_CHECKED(FVRFullScreenUserWidget_PostProcess, WindowVisibility);
		static FName NAME_ReceiveHardwareInput = GET_MEMBER_NAME_CHECKED(FVRFullScreenUserWidget_PostProcess, bReceiveHardwareInput);
		static FName NAME_RenderTargetBackgroundColor = GET_MEMBER_NAME_CHECKED(FVRFullScreenUserWidget_PostProcess, RenderTargetBackgroundColor);
		static FName NAME_RenderTargetBlendMode = GET_MEMBER_NAME_CHECKED(FVRFullScreenUserWidget_PostProcess, RenderTargetBlendMode);
		static FName NAME_PostProcessTintColorAndOpacity = GET_MEMBER_NAME_CHECKED(FVRFullScreenUserWidget_PostProcess, PostProcessTintColorAndOpacity);
		static FName NAME_PostProcessOpacityFromTexture = GET_MEMBER_NAME_CHECKED(FVRFullScreenUserWidget_PostProcess, PostProcessOpacityFromTexture);

		if (Property->GetFName() == NAME_WidgetClass
			|| Property->GetFName() == NAME_EditorDisplayType
			|| Property->GetFName() == NAME_PostProcessMaterial
			|| Property->GetFName() == NAME_WidgetDrawSize
			|| Property->GetFName() == NAME_WindowFocusable
			|| Property->GetFName() == NAME_WindowVisibility
			|| Property->GetFName() == NAME_ReceiveHardwareInput
			|| Property->GetFName() == NAME_RenderTargetBackgroundColor
			|| Property->GetFName() == NAME_RenderTargetBlendMode
			|| Property->GetFName() == NAME_PostProcessTintColorAndOpacity
			|| Property->GetFName() == NAME_PostProcessOpacityFromTexture)
		{
			bool bWasRequestedDisplay = bDisplayRequested;
			UWorld* CurrentWorld = World.Get();
			Hide();
			if (bWasRequestedDisplay && CurrentWorld)
			{
				Display(CurrentWorld);
			}
		}
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

#undef LOCTEXT_NAMESPACE
