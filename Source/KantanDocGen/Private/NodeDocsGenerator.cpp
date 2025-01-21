// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Copyright (C) 2016-2017 Cameron Angus. All Rights Reserved.

#pragma once

#include "NodeDocsGenerator.h"
#include "KantanDocGenLog.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "NodeFactory.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintBoundNodeSpawner.h"
#include "BlueprintComponentNodeSpawner.h"
#include "BlueprintEventNodeSpawner.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Message.h"
#include "HighResScreenshot.h"
#include "XmlFile.h"
#include "Slate/WidgetRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "ThreadingHelpers.h"
#include "Stats/StatsMisc.h"
#include "Runtime/ImageWriteQueue/Public/ImageWriteTask.h"

FNodeDocsGenerator::~FNodeDocsGenerator()
{
	CleanUp();
}

bool FNodeDocsGenerator::GT_Init(FString const& InDocsTitle, FString const& InOutputDir, UClass* BlueprintContextClass)
{
	DummyBP = CastChecked< UBlueprint >(FKismetEditorUtilities::CreateBlueprint(
		BlueprintContextClass,
		::GetTransientPackage(),
		NAME_None,
		EBlueprintType::BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		NAME_None
	));
	if(!DummyBP.IsValid())
	{
		return false;
	}

	Graph = FBlueprintEditorUtils::CreateNewGraph(DummyBP.Get(), TEXT("TempoGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

	DummyBP->AddToRoot();
	Graph->AddToRoot();

	GraphPanel = SNew(SGraphPanel)
		.GraphObj(Graph.Get())
		;
	// We want full detail for rendering, passing a super-high zoom value will guarantee the highest LOD.
	GraphPanel->RestoreViewSettings(FVector2D(0, 0), 10.0f);

	DocsTitle = InDocsTitle;

	IndexXml = InitIndexXml(DocsTitle);
	ClassDocsMap.Empty();

	OutputDir = InOutputDir;

	return true;
}

UK2Node* FNodeDocsGenerator::GT_InitializeForSpawner(UBlueprintNodeSpawner* Spawner, UObject* SourceObject, FNodeProcessingState& OutState)
{
	if(!IsSpawnerDocumentable(Spawner, SourceObject->IsA< UBlueprint >()))
	{
		return nullptr;
	}

	// Spawn an instance into the graph
	auto NodeInst = Spawner->Invoke(Graph.Get(), IBlueprintNodeBinder::FBindingSet{}, FVector2D(0, 0));

	// Currently Blueprint nodes only
	auto K2NodeInst = Cast< UK2Node >(NodeInst);

	if(K2NodeInst == nullptr)
	{
		UE_LOG(LogKantanDocGen, Warning, TEXT("Failed to create node from spawner of class %s with node class %s."), *Spawner->GetClass()->GetName(), Spawner->NodeClass ? *Spawner->NodeClass->GetName() : TEXT("None"));
		return nullptr;
	}

	auto AssociatedClass = MapToAssociatedClass(K2NodeInst, SourceObject);

	if(!ClassDocsMap.Contains(AssociatedClass))
	{
		// New class xml file needs adding
		ClassDocsMap.Add(AssociatedClass, InitClassDocXml(AssociatedClass));
		// Also update the index xml
		UpdateIndexDocWithClass(IndexXml.Get(), AssociatedClass);
	}
	
	OutState = FNodeProcessingState();
	OutState.ClassDocXml = ClassDocsMap.FindChecked(AssociatedClass);
	OutState.ClassDocsPath = OutputDir / GetClassDocId(AssociatedClass);

	return K2NodeInst;
}

bool FNodeDocsGenerator::GT_Finalize(FString OutputPath)
{
	if(!SaveClassDocXml(OutputPath))
	{
		return false;
	}

	if(!SaveIndexXml(OutputPath))
	{
		return false;
	}

	return true;
}

void FNodeDocsGenerator::CleanUp()
{
	if(GraphPanel.IsValid())
	{
		GraphPanel.Reset();
	}

	if(DummyBP.IsValid())
	{
		DummyBP->RemoveFromRoot();
		DummyBP.Reset();
	}
	if(Graph.IsValid())
	{
		Graph->RemoveFromRoot();
		Graph.Reset();
	}
}

// FColor ApplyGamma(FColor Color, float Gamma)
// {
// 	float InvGamma = 1.0f / Gamma;
// 	return FColor(
// 		(uint8)(pow(Color.R / 255.0f, InvGamma) * 255.0f),
// 		(uint8)(pow(Color.G / 255.0f, InvGamma) * 255.0f),
// 		(uint8)(pow(Color.B / 255.0f, InvGamma) * 255.0f),
// 		Color.A // Alpha channel remains unchanged
// 	);
// }

bool FNodeDocsGenerator::GenerateNodeImage(UEdGraphNode* Node, FNodeProcessingState& State)
{
	SCOPE_SECONDS_COUNTER(GenerateNodeImageTime);

	const FVector2D DrawSize(1024.0f, 1024.0f);

	bool bSuccess = false;

	AdjustNodeForSnapshot(Node);

	FString NodeName = GetNodeDocId(Node);

	FIntRect Rect;

	TUniquePtr<TImagePixelData<FColor>> PixelData;

	bSuccess = DocGenThreads::RunOnGameThreadRetVal([this, Node, DrawSize, &Rect, &PixelData]
	{
		auto NodeWidget = FNodeFactory::CreateNodeWidget(Node);
		NodeWidget->SetOwner(GraphPanel.ToSharedRef());

		const bool bUseGammaCorrection = false;
		FWidgetRenderer Renderer(bUseGammaCorrection);
		Renderer.SetIsPrepassNeeded(true);
		auto RenderTarget = Renderer.DrawWidget(NodeWidget.ToSharedRef(), DrawSize);
		if (!RenderTarget)
		{
			UE_LOG(LogKantanDocGen, Error, TEXT("Failed to create RenderTarget."));
			return false;
		}

		auto Desired = NodeWidget->GetDesiredSize();
	
		int32 DesiredX = (int32)Desired.X;
		int32 DesiredY = (int32)Desired.Y;
		// FIntRect Rect(0, 0, DesiredX, DesiredY);
		Rect = FIntRect(0, 0, DesiredX, DesiredY);
		FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
		if (!RTResource)
		{
			UE_LOG(LogKantanDocGen, Error, TEXT("Failed to get RenderTarget resource."));
			return false;
		}
		FReadSurfaceDataFlags ReadPixelFlags(RCM_UNorm);
		ReadPixelFlags.SetLinearToGamma(false); // @TODO: is this gamma correction, or something else?

		PixelData = MakeUnique<TImagePixelData<FColor>>(FIntPoint(DesiredX, DesiredY));
		PixelData->Pixels.SetNumUninitialized(DesiredX* DesiredY);
		if(RTResource->ReadPixelsPtr(PixelData->Pixels.GetData(), ReadPixelFlags, Rect) == false)
		{
			UE_LOG(LogKantanDocGen, Warning, TEXT("Failed to read pixels for node image."));
			return false;
		}
		
		if (RenderTarget)
		{
			RenderTarget->ReleaseResource();
			RenderTarget->MarkPendingKill(); // Optional: Ensure it's marked for GC
			RenderTarget = nullptr;
		}

		// const float DesiredGamma = 2.2f; // Set your desired gamma value
		// UE_LOG(LogKantanDocGen, Warning, TEXT("Gamma %f"), RTResource->GetDisplayGamma());
		// for (FColor& Pixel : PixelData->Pixels)
		// {
		// 	Pixel = ApplyGamma(Pixel, DesiredGamma);
		// }

		return true;
	});

	if(!bSuccess)
	{
		return false;
	}

	State.RelImageBasePath = TEXT("../img");
	FString ImageBasePath = State.ClassDocsPath / TEXT("img");// State.RelImageBasePath;
	FString ImgFilename = FString::Printf(TEXT("nd_img_%s.png"), *NodeName);
	FString ScreenshotSaveName = ImageBasePath / ImgFilename;

	TUniquePtr<FImageWriteTask> ImageTask = MakeUnique<FImageWriteTask>();
	ImageTask->PixelData = MoveTemp(PixelData);
	ImageTask->Filename = ScreenshotSaveName;
	ImageTask->Format = EImageFormat::PNG;
	ImageTask->CompressionQuality = (int32)EImageCompressionQuality::Uncompressed;
	ImageTask->bOverwriteFile = true;
	ImageTask->PixelPreProcessors.Add(TAsyncGammaCorrect<FColor>(1/2.2f));
	// ImageTask->PixelPreProcessors.Add(TAsyncAlphaWrite<FColor>(255));
	
	if(ImageTask->RunTask())
	{
		// Success!
		bSuccess = true;
		State.ImageFilename = ImgFilename;
	}
	else
	{
		UE_LOG(LogKantanDocGen, Warning, TEXT("Failed to save screenshot image for node: %s"), *NodeName);
	}

	return bSuccess;
}

inline FString WrapAsCDATA(FString const& InString)
{
	return TEXT("<![CDATA[") + InString + TEXT("]]>");
}

inline FXmlNode* AppendChild(FXmlNode* Parent, FString const& Name)
{
	Parent->AppendChildNode(Name, FString());
	return Parent->GetChildrenNodes().Last();
}

inline FXmlNode* AppendChildRaw(FXmlNode* Parent, FString const& Name, FString const& TextContent)
{
	Parent->AppendChildNode(Name, TextContent);
	return Parent->GetChildrenNodes().Last();
}

inline FXmlNode* AppendChildCDATA(FXmlNode* Parent, FString const& Name, FString const& TextContent)
{
	Parent->AppendChildNode(Name, WrapAsCDATA(TextContent));
	return Parent->GetChildrenNodes().Last();
}

/*
* from edgraphschema
*/

const UScriptStruct* VectorStruct = nullptr;
const UScriptStruct* RotatorStruct = nullptr;
const UScriptStruct* TransformStruct = nullptr;
const UScriptStruct* LinearColorStruct = nullptr;
const UScriptStruct* ColorStruct = nullptr;

FText TerminalTypeToText(const FName Category, const FName SubCategory, UObject* SubCategoryObject, bool bIsWeakPtr)
{
	FText PropertyText;

	if (VectorStruct == nullptr)
	{
		VectorStruct = TBaseStructure<FVector>::Get();
		RotatorStruct = TBaseStructure<FRotator>::Get();
		TransformStruct = TBaseStructure<FTransform>::Get();
		LinearColorStruct = TBaseStructure<FLinearColor>::Get();
		ColorStruct = TBaseStructure<FColor>::Get();
	}
if (SubCategory != UEdGraphSchema_K2::PSC_Bitmask && SubCategoryObject != nullptr)
	{
		if (Category == UEdGraphSchema_K2::PC_Byte)
		{
			FFormatNamedArguments Args;
			Args.Add(TEXT("EnumName"), FText::FromString(SubCategoryObject->GetName()));
			PropertyText = FText::Format(NSLOCTEXT("NodeDocsGen", "EnumAsText", "{EnumName} Enum"), Args);
		}
		else
		{
			FString SubCategoryObjName;
			if (UField* SubCategoryField = Cast<UField>(SubCategoryObject))
			{
				// SubCategoryObjName = SubCategoryField->GetDisplayNameText().ToString();
				SubCategoryObjName = SubCategoryObject->GetName();
			}
			else
			{
				SubCategoryObjName = SubCategoryObject->GetName();
			}

			if (!bIsWeakPtr)
			{
				UClass* PSCOAsClass = Cast<UClass>(SubCategoryObject);

				if (PSCOAsClass != nullptr)
				{
					SubCategoryObjName = PSCOAsClass->GetPrefixCPP() + SubCategoryObjName;
				}
					// UE_LOG(LogTemp, Error, TEXT("%s prefix: %s subobj_prefix: %s"), *SubCategoryObjName, PSCOAsClass->GetPrefixCPP(), SubCategoryObject->GetClass()->GetPrefixCPP());
				const bool bIsInterface = PSCOAsClass && PSCOAsClass->HasAnyClassFlags(CLASS_Interface);

				FFormatNamedArguments Args;
				// Args.Add(TEXT("ObjectName"), FText::FromString(FName::NameToDisplayString(SubCategoryObjName, /*bIsBool =*/false)));
				// TODO: don't make links here, make an info struct for the pin
				// SubCategoryObjName = SubCategoryObject->GetClass()->GetPrefixCPP() + SubCategoryObjName;
				Args.Add(TEXT("ObjectName"), FText::FromString(L"[["+SubCategoryObjName+L"]]"));

				// Don't display the category for "well-known" struct types
				if (Category == UEdGraphSchema_K2::PC_Struct && (SubCategoryObject == VectorStruct || SubCategoryObject == RotatorStruct || SubCategoryObject == TransformStruct))
				{
					PropertyText = FText::Format(NSLOCTEXT("NodeDocsGen", "ObjectAsTextWithoutCategory", "{ObjectName}"), Args);
				}
				// If this is a raw UObject reference don't display Object twice
				else if (((Category == UEdGraphSchema_K2::PC_Object) || (Category == UEdGraphSchema_K2::PC_SoftObject)) && (SubCategoryObject == UObject::StaticClass()))
				{
					Args.Add(TEXT("Category"), UEdGraphSchema_K2::GetCategoryText(Category));
					PropertyText = FText::Format(NSLOCTEXT("NodeDocsGen", "ObjectAsJustCategory", "{Category}"), Args);
				}
				else
				{
					Args.Add(TEXT("Category"), (!bIsInterface ? UEdGraphSchema_K2::GetCategoryText(Category) : UEdGraphSchema_K2::GetCategoryText(UEdGraphSchema_K2::PC_Interface)));
					PropertyText = FText::Format(NSLOCTEXT("NodeDocsGen", "ObjectAsText", "{ObjectName} {Category}"), Args);
				}
			}
			else
			{
				FFormatNamedArguments Args;
				Args.Add(TEXT("Category"), FText::FromName(Category));
				Args.Add(TEXT("ObjectName"), FText::FromString(SubCategoryObject->GetClass()->GetPrefixCPP() + SubCategoryObjName));
				PropertyText = FText::Format(NSLOCTEXT("NodeDocsGen", "WeakPtrAsText", "{ObjectName} Weak {Category}"), Args);
			}
		}
	}
	else if (!SubCategory.IsNone())
	{
		FFormatNamedArguments Args;
		Args.Add(TEXT("Category"), UEdGraphSchema_K2::GetCategoryText(Category));
		// Args.Add(TEXT("ObjectName"), FText::FromString(FName::NameToDisplayString(SubCategory.ToString(), false)));
		Args.Add(TEXT("ObjectName"), FText::FromString(SubCategory.ToString()));
		PropertyText = FText::Format(NSLOCTEXT("NodeDocsGen", "ObjectAsText", "{ObjectName} {Category}"), Args);
	}
	else
	{
		PropertyText = UEdGraphSchema_K2::GetCategoryText(Category);
	}

	return PropertyText;
}

FText TypeToText(const FEdGraphPinType& Type)
{
	FText PropertyText = TerminalTypeToText(Type.PinCategory, Type.PinSubCategory, Type.PinSubCategoryObject.Get(), Type.bIsWeakPointer);

	if (Type.IsMap())
	{
		FFormatNamedArguments Args;
		Args.Add(TEXT("KeyTitle"), PropertyText);
		FText ValueText = TerminalTypeToText(Type.PinValueType.TerminalCategory, Type.PinValueType.TerminalSubCategory, Type.PinValueType.TerminalSubCategoryObject.Get(), Type.PinValueType.bTerminalIsWeakPointer);
		Args.Add(TEXT("ValueTitle"), ValueText);
		PropertyText = FText::Format(NSLOCTEXT("NodeDocsGen", "MapAsText", "Map of {KeyTitle}s to {ValueTitle}s"), Args);
	}
	else if (Type.IsSet())
	{
		FFormatNamedArguments Args;
		Args.Add(TEXT("PropertyTitle"), PropertyText);
		PropertyText = FText::Format(NSLOCTEXT("NodeDocsGen", "SetAsText", "Set of {PropertyTitle}s"), Args);
	}
	else if (Type.IsArray())
	{
		FFormatNamedArguments Args;
		Args.Add(TEXT("PropertyTitle"), PropertyText);
		PropertyText = FText::Format(NSLOCTEXT("NodeDocsGen", "ArrayAsText", "Array of {PropertyTitle}s"), Args);
	}
	else if (Type.bIsReference)
	{
		FFormatNamedArguments Args;
		Args.Add(TEXT("PropertyTitle"), PropertyText);
		PropertyText = FText::Format(NSLOCTEXT("NodeDocsGen", "PropertyByRef", "{PropertyTitle} (by ref)"), Args);
	}

	return PropertyText;
}

/*
* /from edgraphschema
*/
// For K2 pins only!
bool ExtractPinInformation(UEdGraphPin* Pin, FString& OutName, FString& OutType, FString& OutDescription)
{
	FString Tooltip;
	Pin->GetOwningNode()->GetPinHoverText(*Pin, Tooltip);
	if(!Tooltip.IsEmpty())
	{
		// @NOTE: This is based on the formatting in UEdGraphSchema_K2::ConstructBasicPinTooltip.
		// If that is changed, this will fail!
		
		auto TooltipPtr = *Tooltip;

		// Parse name line
		FParse::Line(&TooltipPtr, OutName);
		// Parse type line
		FParse::Line(&TooltipPtr, OutType);

		// Currently there is an empty line here, but FParse::Line seems to gobble up empty lines as part of the previous call.
		// Anyway, attempting here to deal with this generically in case that weird behaviour changes.
		while(*TooltipPtr == TEXT('\n'))
		{
			FString Buf;
			FParse::Line(&TooltipPtr, Buf);
		}

		// What remains is the description
		OutDescription = TooltipPtr;
	}

	// @NOTE: Currently overwriting the name and type as suspect this is more robust to future engine changes.

	OutName = Pin->GetDisplayName().ToString();
	if(OutName.IsEmpty() && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
	{
		OutName = Pin->Direction == EEdGraphPinDirection::EGPD_Input ? TEXT("In") : TEXT("Out");
	}

	OutType = TypeToText(Pin->PinType).ToString();
	// OutType = UEdGraphSchema_K2::TypeToText(Pin->PinType).ToString();

	return true;
}

TSharedPtr< FXmlFile > FNodeDocsGenerator::InitIndexXml(FString const& IndexTitle)
{
	const FString FileTemplate = R"xxx(<?xml version="1.0" encoding="UTF-8"?>
<root></root>)xxx";

	TSharedPtr< FXmlFile > File = MakeShared< FXmlFile >(FileTemplate, EConstructMethod::ConstructFromBuffer);
	auto Root = File->GetRootNode();

	AppendChildCDATA(Root, TEXT("display_name"), IndexTitle);
	AppendChild(Root, TEXT("classes"));

	return File;
}

TSharedPtr< FXmlFile > FNodeDocsGenerator::InitClassDocXml(UClass* Class)
{
	const FString FileTemplate = R"xxx(<?xml version="1.0" encoding="UTF-8"?>
<root></root>)xxx";

	TSharedPtr< FXmlFile > File = MakeShared< FXmlFile >(FileTemplate, EConstructMethod::ConstructFromBuffer);
	auto Root = File->GetRootNode();

	AppendChildCDATA(Root, TEXT("docs_name"), DocsTitle);
	AppendChildCDATA(Root, TEXT("id"), GetClassDocId(Class));
	AppendChildCDATA(Root, TEXT("display_name"), FBlueprintEditorUtils::GetFriendlyClassDisplayName(Class).ToString());
	AppendChild(Root, TEXT("nodes"));

	return File;
}

bool FNodeDocsGenerator::UpdateIndexDocWithClass(FXmlFile* DocFile, UClass* Class)
{
	auto ClassId = GetClassDocId(Class);
	auto Classes = DocFile->GetRootNode()->FindChildNode(TEXT("classes"));
	auto ClassElem = AppendChild(Classes, TEXT("class"));
	AppendChildCDATA(ClassElem, TEXT("id"), ClassId);
	AppendChildCDATA(ClassElem, TEXT("display_name"), FBlueprintEditorUtils::GetFriendlyClassDisplayName(Class).ToString());
	return true;
}

bool FNodeDocsGenerator::UpdateClassDocWithNode(FXmlFile* DocFile, UEdGraphNode* Node)
{
	auto NodeId = GetNodeDocId(Node);
	auto Nodes = DocFile->GetRootNode()->FindChildNode(TEXT("nodes"));
	auto NodeElem = AppendChild(Nodes, TEXT("node"));
	AppendChildCDATA(NodeElem, TEXT("id"), NodeId);
	AppendChildCDATA(NodeElem, TEXT("shorttitle"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	return true;
}

inline bool ShouldDocumentPin(UEdGraphPin* Pin)
{
	return !Pin->bHidden;
}


#include "JsonUtilities/Public/JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"



bool FNodeDocsGenerator::GenerateNodeMarkdown(const FNodeDocsData &NodeData, const FString& MarkdownFilePath)
{
	FString MarkdownContent;

	// Set Tags
	MarkdownContent += TEXT("---\n");
	MarkdownContent += FString::Printf(TEXT("OwnerClass: \"[[%s]]\"\n"), *NodeData.ClassId);
	if (!NodeData.Category.IsEmpty())
		MarkdownContent += FString::Printf(TEXT("NodeCategory: %s\n"), *NodeData.Category);
	MarkdownContent += FString::Printf(TEXT("aliases:\n  - %s\n"), *NodeData.Description);
	MarkdownContent += TEXT("---\n");

	// Add Image
	if (!NodeData.ImgPath.IsEmpty())
		MarkdownContent += FString::Printf(TEXT("## Preview\n\n![Image](%s)\n"), *NodeData.ImgPath);
	// MarkdownContent += FString::Printf(TEXT("## Preview\n![Image](%s)\n"), *NodeData.ImgPath);
	
	// Add Inputs Table
	MarkdownContent += TEXT("## IO\n");
	if (NodeData.Inputs.Num() > 0)
	{
		// MarkdownContent += TEXT("## Inputs\n| Name | Type |\n|------|------|\n");
		MarkdownContent += TEXT("| **Input** | **Type** |\n|------|------|\n");
		for (const FNodeDocsParam& Input : NodeData.Inputs)
		{
			MarkdownContent += FString::Printf(TEXT("| %s | %s |\n"),
											   *Input.Name, *Input.Type);
		}
		MarkdownContent += TEXT("\n");
	}
	
	// Add Outputs Table
	if (NodeData.Outputs.Num() > 0)
	{
		// MarkdownContent += TEXT("## Outputs\n| Name | Type |\n|-------|------|\n");
		MarkdownContent += TEXT("| **Output** | **Type** |\n|-------|------|\n");
		for (const FNodeDocsParam& Output : NodeData.Outputs)
		{
			MarkdownContent += FString::Printf(TEXT("| %s | %s |\n"),
											   *Output.Name, *Output.Type);
		}
	}

	// Save to file
	if (!FFileHelper::SaveStringToFile(MarkdownContent, *MarkdownFilePath))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to save Markdown file: %s"), *MarkdownFilePath);
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("Markdown file saved successfully: %s"), *MarkdownFilePath);
	return true;
}

bool FNodeDocsGenerator::GenerateNodeJsonDocs(UK2Node* Node, FNodeProcessingState& State)
{
	auto BPClass = Node->GetBlueprintClassFromNode();
	SCOPE_SECONDS_COUNTER(GenerateNodeJsonDocsTime);
	auto NodeDocsPath = State.ClassDocsPath / TEXT("nodes");
	FString DocFilePath = NodeDocsPath / (GetNodeDocId(Node) + TEXT(".json"));
	FString MarkdownFilePath = NodeDocsPath / (GetNodeDocId(Node) + TEXT(".md"));
	FNodeDocsData NodeData;
	NodeData.DocsName = DocsTitle;
	
	auto AssociatedClass = MapToAssociatedClass(Node, nullptr);
	if (AssociatedClass == nullptr)
	{
		// UE_LOG(LogTemp, Error, TEXT("Failed to find associatedClass for %s"), *DocFilePath);
		NodeData.ClassId = State.ClassDocXml->GetRootNode()->FindChildNode(TEXT("id"))->GetContent();
		NodeData.ClassName = State.ClassDocXml->GetRootNode()->FindChildNode(TEXT("display_name"))->GetContent();
		NodeData.ClassId.RemoveFromStart(TEXT("<![CDATA["));
		NodeData.ClassId.RemoveFromEnd(TEXT("]]>"));
		// return false;
	}
	else
	{
		NodeData.ClassId = GetClassDocId(AssociatedClass);
		NodeData.ClassName = FBlueprintEditorUtils::GetFriendlyClassDisplayName(AssociatedClass).ToString();
	}

	NodeData.ClassId.RemoveFromStart(TEXT("SKEL_"));
	// NodeData.ClassId.RemoveFromEnd(TEXT("_C"));
	
	NodeData.ShortTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString().TrimEnd();

	FString NodeFullTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	auto TargetIdx = NodeFullTitle.Find(TEXT("Target is "), ESearchCase::CaseSensitive);
	if(TargetIdx != INDEX_NONE)
	{
		NodeFullTitle = NodeFullTitle.Left(TargetIdx).TrimEnd();
	}
	NodeData.FullTitle = NodeFullTitle;

	FString NodeDesc = Node->GetTooltipText().ToString();
	TargetIdx = NodeDesc.Find(TEXT("Target is "), ESearchCase::CaseSensitive);
	if(TargetIdx != INDEX_NONE)
	{
		NodeDesc = NodeDesc.Left(TargetIdx).TrimEnd();
	}
	NodeData.Description = NodeDesc;

	NodeData.ImgPath = State.RelImageBasePath / State.ImageFilename;
	NodeData.Category = Node->GetMenuCategory().ToString();

	for(auto Pin : Node->Pins)
		if(ShouldDocumentPin(Pin))
		{
			FString PinName, PinType, PinDesc;
			ExtractPinInformation(Pin, PinName, PinType, PinDesc);
			if(Pin->Direction == EEdGraphPinDirection::EGPD_Input)
				NodeData.Inputs.Add(FNodeDocsParam(PinName, PinType, PinDesc));
			else if(Pin->Direction == EEdGraphPinDirection::EGPD_Output)
				NodeData.Outputs.Add(FNodeDocsParam(PinName, PinType, PinDesc));
		}

	GenerateNodeMarkdown(NodeData, MarkdownFilePath);
	
	if (false)
	{
		TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();
		if (!FJsonObjectConverter::UStructToJsonObject(FNodeDocsData::StaticStruct(), &NodeData, JsonObject.ToSharedRef(), 0, 0))
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to convert data to JSON."));
			return false;
		}

		// Serialize JSON to string
		FString JsonOutputString;
		TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&JsonOutputString);
		if (!FJsonSerializer::Serialize(JsonObject.ToSharedRef(), JsonWriter))
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to serialize JSON."));
			return false;
		}

		// Save JSON to file
		if (!FFileHelper::SaveStringToFile(JsonOutputString, *DocFilePath))
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to save JSON file: %s"), *DocFilePath);
			return false;
		}

		UE_LOG(LogTemp, Log, TEXT("JSON saved successfully: %s"), *DocFilePath);		
	}
	return true;
}

bool FNodeDocsGenerator::GenerateNodeDocs(UK2Node* Node, FNodeProcessingState& State)
{
	SCOPE_SECONDS_COUNTER(GenerateNodeDocsTime);

	// GenerateNodeJsonDocs(Node, State);

	auto NodeDocsPath = State.ClassDocsPath / TEXT("nodes");
	FString DocFilePath = NodeDocsPath / (GetNodeDocId(Node) + TEXT(".xml"));

	const FString FileTemplate = R"xxx(<?xml version="1.0" encoding="UTF-8"?>
<root></root>)xxx";

	FXmlFile File(FileTemplate, EConstructMethod::ConstructFromBuffer);
	auto Root = File.GetRootNode();
	
	AppendChildCDATA(Root, TEXT("docs_name"), DocsTitle);
	// Since we pull these from the class xml file, the entries are already CDATA wrapped
	AppendChildRaw(Root, TEXT("class_id"), State.ClassDocXml->GetRootNode()->FindChildNode(TEXT("id"))->GetContent());//GetClassDocId(Class));
	AppendChildRaw(Root, TEXT("class_name"), State.ClassDocXml->GetRootNode()->FindChildNode(TEXT("display_name"))->GetContent());// FBlueprintEditorUtils::GetFriendlyClassDisplayName(Class).ToString());

	FString NodeShortTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	AppendChildCDATA(Root, TEXT("shorttitle"), NodeShortTitle.TrimEnd());

	FString NodeFullTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	auto TargetIdx = NodeFullTitle.Find(TEXT("Target is "), ESearchCase::CaseSensitive);
	if(TargetIdx != INDEX_NONE)
	{
		NodeFullTitle = NodeFullTitle.Left(TargetIdx).TrimEnd();
	}
	AppendChildCDATA(Root, TEXT("fulltitle"), NodeFullTitle);

	FString NodeDesc = Node->GetTooltipText().ToString();
	TargetIdx = NodeDesc.Find(TEXT("Target is "), ESearchCase::CaseSensitive);
	if(TargetIdx != INDEX_NONE)
	{
		NodeDesc = NodeDesc.Left(TargetIdx).TrimEnd();
	}
	AppendChildCDATA(Root, TEXT("description"), NodeDesc);
	AppendChildCDATA(Root, TEXT("imgpath"), State.RelImageBasePath / State.ImageFilename);
	AppendChildCDATA(Root, TEXT("category"), Node->GetMenuCategory().ToString());
	
	auto Inputs = AppendChild(Root, TEXT("inputs"));
	for(auto Pin : Node->Pins)
	{
		if(Pin->Direction == EEdGraphPinDirection::EGPD_Input)
		{
			if(ShouldDocumentPin(Pin))
			{
				auto Input = AppendChild(Inputs, TEXT("param"));

				FString PinName, PinType, PinDesc;
				ExtractPinInformation(Pin, PinName, PinType, PinDesc);

				AppendChildCDATA(Input, TEXT("name"), PinName);
				AppendChildCDATA(Input, TEXT("type"), PinType);
				AppendChildCDATA(Input, TEXT("description"), PinDesc);
			}
		}
	}

	auto Outputs = AppendChild(Root, TEXT("outputs"));
	for(auto Pin : Node->Pins)
	{
		if(Pin->Direction == EEdGraphPinDirection::EGPD_Output)
		{
			if(ShouldDocumentPin(Pin))
			{
				auto Output = AppendChild(Outputs, TEXT("param"));

				FString PinName, PinType, PinDesc;
				ExtractPinInformation(Pin, PinName, PinType, PinDesc);

				AppendChildCDATA(Output, TEXT("name"), PinName);
				AppendChildCDATA(Output, TEXT("type"), PinType);
				AppendChildCDATA(Output, TEXT("description"), PinDesc);
			}
		}
	}

	if(!File.Save(DocFilePath))
	{
		return false;
	}

	if(!UpdateClassDocWithNode(State.ClassDocXml.Get(), Node))
	{
		return false;
	}
	
	return true;
}

bool FNodeDocsGenerator::SaveIndexXml(FString const& OutDir)
{
	auto Path = OutDir / TEXT("index.xml");
	IndexXml->Save(Path);

	return true;
}

bool FNodeDocsGenerator::SaveClassDocXml(FString const& OutDir)
{
	for(auto const& Entry : ClassDocsMap)
	{
		auto ClassId = GetClassDocId(Entry.Key.Get());
		auto Path = OutDir / ClassId / (ClassId + TEXT(".xml"));
		Entry.Value->Save(Path);
	}

	return true;
}


void FNodeDocsGenerator::AdjustNodeForSnapshot(UEdGraphNode* Node)
{
	// Hide default value box containing 'self' for Target pin
	if(auto K2_Schema = Cast< UEdGraphSchema_K2 >(Node->GetSchema()))
	{
		if(auto TargetPin = Node->FindPin(K2_Schema->PN_Self))
		{
			TargetPin->bDefaultValueIsIgnored = true;
		}
	}
}

FString FNodeDocsGenerator::GetClassDocId(UClass* Class)
{
	return Class->GetName();
}

FString FNodeDocsGenerator::GetNodeDocId(UEdGraphNode* Node)
{
	// @TODO: Not sure this is right thing to use
	return Node->GetDocumentationExcerptName();
}


#include "BlueprintVariableNodeSpawner.h"
#include "BlueprintDelegateNodeSpawner.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"

/*
This takes a graph node object and attempts to map it to the class which the node conceptually belong to.
If there is no special mapping for the node, the function determines the class from the source object.
*/
UClass* FNodeDocsGenerator::MapToAssociatedClass(UK2Node* NodeInst, UObject* Source)
{
	// For nodes derived from UK2Node_CallFunction, associate with the class owning the called function.
	if(auto FuncNode = Cast< UK2Node_CallFunction >(NodeInst))
	{
		auto Func = FuncNode->GetTargetFunction();
		if(Func)
		{
			return Func->GetOwnerClass();
		}
	}

	// Default fallback
	if(auto SourceClass = Cast< UClass >(Source))
	{
		return SourceClass;
	}
	else if(auto SourceBP = Cast< UBlueprint >(Source))
	{
		return SourceBP->GeneratedClass;
	}
	else
	{
		return nullptr;
	}
}

bool FNodeDocsGenerator::IsSpawnerDocumentable(UBlueprintNodeSpawner* Spawner, bool bIsBlueprint)
{
	// Spawners of or deriving from the following classes will be excluded
	static const TSubclassOf< UBlueprintNodeSpawner > ExcludedSpawnerClasses[] = {
		UBlueprintVariableNodeSpawner::StaticClass(),
		UBlueprintDelegateNodeSpawner::StaticClass(),
		UBlueprintBoundNodeSpawner::StaticClass(),
		UBlueprintComponentNodeSpawner::StaticClass(),
	};

	// Spawners of or deriving from the following classes will be excluded in a blueprint context
	static const TSubclassOf< UBlueprintNodeSpawner > BlueprintOnlyExcludedSpawnerClasses[] = {
		UBlueprintEventNodeSpawner::StaticClass(),
	};

	// Spawners for nodes of these types (or their subclasses) will be excluded
	static const TSubclassOf< UK2Node > ExcludedNodeClasses[] = {
		UK2Node_DynamicCast::StaticClass(),
		UK2Node_Message::StaticClass(),
	};

	// Function spawners for functions with any of the following metadata tags will also be excluded
	static const FName ExcludedFunctionMeta[] = {
		TEXT("BlueprintAutocast")
	};

	static const uint32 PermittedAccessSpecifiers = (FUNC_Public | FUNC_Protected);


	for(auto ExclSpawnerClass : ExcludedSpawnerClasses)
	{
		if(Spawner->IsA(ExclSpawnerClass))
		{
			return false;
		}
	}

	if(bIsBlueprint)
	{
		for(auto ExclSpawnerClass : BlueprintOnlyExcludedSpawnerClasses)
		{
			if(Spawner->IsA(ExclSpawnerClass))
			{
				return false;
			}
		}
	}

	for(auto ExclNodeClass : ExcludedNodeClasses)
	{
		if(Spawner->NodeClass->IsChildOf(ExclNodeClass))
		{
			return false;
		}
	}

	if(auto FuncSpawner = Cast< UBlueprintFunctionNodeSpawner >(Spawner))
	{
		auto Func = FuncSpawner->GetFunction();

		// @NOTE: We exclude based on access level, but only if this is not a spawner for a blueprint event
		// (custom events do not have any access specifiers)
		if((Func->FunctionFlags & FUNC_BlueprintEvent) == 0 && (Func->FunctionFlags & PermittedAccessSpecifiers) == 0)
		{
			return false;
		}

		for(auto const& Meta : ExcludedFunctionMeta)
		{
			if(Func->HasMetaData(Meta))
			{
				return false;
			}
		}
	}

	return true;
}

