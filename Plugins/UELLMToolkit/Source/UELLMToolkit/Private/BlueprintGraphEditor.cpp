// Copyright Natali Caggiano. All Rights Reserved.

#include "BlueprintGraphEditor.h"
#include "UnrealClaudeModule.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Self.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "EdGraphSchema_K2.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Animation/AnimInstance.h"
#include "HAL/PlatformAtomics.h"
// AnimGraph node types
#include "AnimGraphNode_ModifyBone.h"
#include "AnimGraphNode_TwoBoneIK.h"
#include "AnimGraphNode_ControlRig.h"
#include "AnimGraphNode_CustomProperty.h"
#include "ControlRig.h"
#include "Animation/AnimationAsset.h"
// Enhanced Input event node
#include "K2Node_EnhancedInputAction.h"
#include "InputAction.h"

// Static member initialization
volatile int32 FBlueprintGraphEditor::NodeIdCounter = 0;
const FString FBlueprintGraphEditor::NodeIdPrefix = TEXT("MCP_ID:");

// ===== Graph Finding =====

UEdGraph* FBlueprintGraphEditor::FindGraph(
	UBlueprint* Blueprint,
	const FString& GraphName,
	bool bFunctionGraph,
	FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return nullptr;
	}

	// Special handling for AnimGraph — search all graphs for UAnimationGraph
	if (GraphName.Equals(TEXT("AnimGraph"), ESearchCase::IgnoreCase))
	{
		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph && Graph->GetFName() == FName("AnimGraph"))
			{
				return Graph;
			}
		}
		OutError = TEXT("AnimGraph not found. Is this an Animation Blueprint?");
		return nullptr;
	}

	// Get the appropriate graph array (UE 5.7 uses TObjectPtr)
	auto& Graphs = bFunctionGraph ? Blueprint->FunctionGraphs : Blueprint->UbergraphPages;

	// If no name specified, return the first graph (default)
	if (GraphName.IsEmpty())
	{
		if (Graphs.Num() > 0 && Graphs[0])
		{
			return Graphs[0];
		}
		OutError = bFunctionGraph ? TEXT("No function graphs found") : TEXT("No event graphs found");
		return nullptr;
	}

	// Search by name
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	// Build list of available graphs for error message
	TArray<FString> AvailableGraphs;
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph)
		{
			AvailableGraphs.Add(Graph->GetName());
		}
	}

	OutError = FString::Printf(TEXT("Graph '%s' not found. Available: %s"),
		*GraphName,
		*FString::Join(AvailableGraphs, TEXT(", ")));
	return nullptr;
}

// ===== Node Management =====

UEdGraphNode* FBlueprintGraphEditor::CreateNode(
	UEdGraph* Graph,
	const FString& NodeType,
	const TSharedPtr<FJsonObject>& NodeParams,
	int32 PosX,
	int32 PosY,
	FString& OutNodeId,
	FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return nullptr;
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (!Blueprint)
	{
		OutError = TEXT("Could not find Blueprint for graph");
		return nullptr;
	}

	UEdGraphNode* NewNode = nullptr;
	FString Context;

	// Dispatch to appropriate creation function
	if (NodeType.Equals(TEXT("CallFunction"), ESearchCase::IgnoreCase))
	{
		FString FunctionName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("function")) : TEXT("");
		FString TargetClass = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("target_class")) : TEXT("");
		Context = FunctionName;
		NewNode = CreateCallFunctionNode(Graph, FunctionName, TargetClass, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("Branch"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("IfThenElse"), ESearchCase::IgnoreCase))
	{
		NewNode = CreateBranchNode(Graph, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("Event"), ESearchCase::IgnoreCase))
	{
		FString EventName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("event")) : TEXT("");
		Context = EventName;
		NewNode = CreateEventNode(Graph, EventName, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("CustomEvent"), ESearchCase::IgnoreCase))
	{
		FString EventName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("event_name")) : TEXT("");
		Context = EventName;
		NewNode = CreateCustomEventNode(Graph, EventName, NodeParams, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("ComponentBoundEvent"), ESearchCase::IgnoreCase))
	{
		FString ComponentName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("component")) : TEXT("");
		FString DelegateName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("delegate")) : TEXT("");
		Context = FString::Printf(TEXT("%s_%s"), *ComponentName, *DelegateName);
		NewNode = CreateComponentBoundEventNode(Graph, Blueprint, ComponentName, DelegateName, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("Self"), ESearchCase::IgnoreCase))
	{
		Context = TEXT("Self");
		NewNode = CreateSelfNode(Graph, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("Cast"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("DynamicCast"), ESearchCase::IgnoreCase))
	{
		FString TargetClass = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("target_class")) : TEXT("");
		Context = TargetClass;
		NewNode = CreateCastNode(Graph, TargetClass, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("MakeStruct"), ESearchCase::IgnoreCase))
	{
		FString StructType = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("struct_type")) : TEXT("");
		Context = StructType;
		NewNode = CreateMakeStructNode(Graph, StructType, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("BreakStruct"), ESearchCase::IgnoreCase))
	{
		FString StructType = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("struct_type")) : TEXT("");
		Context = StructType;
		NewNode = CreateBreakStructNode(Graph, StructType, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("EnhancedInputAction"), ESearchCase::IgnoreCase))
	{
		FString ActionPath = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("action_path")) : TEXT("");
		Context = ActionPath;
		NewNode = CreateEnhancedInputActionNode(Graph, ActionPath, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("VariableGet"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("GetVariable"), ESearchCase::IgnoreCase))
	{
		FString VariableName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("variable")) : TEXT("");
		Context = VariableName;
		NewNode = CreateVariableGetNode(Graph, Blueprint, VariableName, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("VariableSet"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("SetVariable"), ESearchCase::IgnoreCase))
	{
		FString VariableName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("variable")) : TEXT("");
		Context = VariableName;
		NewNode = CreateVariableSetNode(Graph, Blueprint, VariableName, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("Sequence"), ESearchCase::IgnoreCase))
	{
		int32 NumOutputs = NodeParams.IsValid() ? (int32)NodeParams->GetNumberField(TEXT("num_outputs")) : 2;
		if (NumOutputs < 2) NumOutputs = 2;
		NewNode = CreateSequenceNode(Graph, NumOutputs, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("Add"), ESearchCase::IgnoreCase) ||
			 NodeType.Equals(TEXT("Subtract"), ESearchCase::IgnoreCase) ||
			 NodeType.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase) ||
			 NodeType.Equals(TEXT("Divide"), ESearchCase::IgnoreCase))
	{
		Context = NodeType;
		NewNode = CreateMathNode(Graph, NodeType, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("PrintString"), ESearchCase::IgnoreCase))
	{
		// Convenience alias for CallFunction with PrintString
		Context = TEXT("PrintString");
		NewNode = CreateCallFunctionNode(Graph, TEXT("PrintString"), TEXT("KismetSystemLibrary"), PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("ModifyBone"), ESearchCase::IgnoreCase))
	{
		FString BoneName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("bone_name")) : TEXT("");
		Context = BoneName;
		NewNode = CreateModifyBoneNode(Graph, BoneName, NodeParams, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("TwoBoneIK"), ESearchCase::IgnoreCase))
	{
		FString BoneName = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("bone_name")) : TEXT("");
		Context = BoneName;
		NewNode = CreateTwoBoneIKNode(Graph, BoneName, NodeParams, PosX, PosY, OutError);
	}
	else if (NodeType.Equals(TEXT("ControlRig"), ESearchCase::IgnoreCase))
	{
		FString RigClass = NodeParams.IsValid() ? NodeParams->GetStringField(TEXT("control_rig_class")) : TEXT("");
		Context = RigClass;
		NewNode = CreateControlRigNode(Graph, RigClass, NodeParams, PosX, PosY, OutError);
	}
	else
	{
		OutError = FString::Printf(TEXT("Unknown node type: '%s'. Supported: CallFunction, Branch, Event, CustomEvent, ComponentBoundEvent, EnhancedInputAction, VariableGet, VariableSet, Sequence, Self, Cast, MakeStruct, BreakStruct, Add, Subtract, Multiply, Divide, PrintString, ModifyBone, TwoBoneIK, ControlRig"), *NodeType);
		return nullptr;
	}

	if (NewNode)
	{
		// Generate and set node ID
		OutNodeId = GenerateNodeId(NodeType, Context, Graph);
		SetNodeId(NewNode, OutNodeId);

		// Mark blueprint as modified
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		UE_LOG(LogUnrealClaude, Log, TEXT("Created node '%s' (type: %s) at (%d, %d)"), *OutNodeId, *NodeType, PosX, PosY);
	}

	return NewNode;
}

bool FBlueprintGraphEditor::DeleteNode(UEdGraph* Graph, const FString& NodeId, FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return false;
	}

	UEdGraphNode* Node = FindNodeById(Graph, NodeId);
	if (!Node)
	{
		OutError = FString::Printf(TEXT("Node '%s' not found"), *NodeId);
		return false;
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);

	// Break all connections first
	Node->BreakAllNodeLinks();

	// Remove from graph
	Graph->RemoveNode(Node);

	if (Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("Deleted node '%s'"), *NodeId);
	return true;
}

UEdGraphNode* FBlueprintGraphEditor::FindNodeById(UEdGraph* Graph, const FString& NodeId)
{
	if (!Graph || NodeId.IsEmpty())
	{
		return nullptr;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && GetNodeId(Node) == NodeId)
		{
			return Node;
		}
	}

	// Fallback: match by UE node name (for pre-existing nodes like AnimGraphNode_Root_0)
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->GetName() == NodeId)
		{
			return Node;
		}
	}

	return nullptr;
}

// ===== Pin & Connection Management =====

bool FBlueprintGraphEditor::ConnectPins(
	UEdGraph* Graph,
	const FString& SourceNodeId,
	const FString& SourcePinName,
	const FString& TargetNodeId,
	const FString& TargetPinName,
	FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return false;
	}

	// Find source node by MCP-generated ID
	UEdGraphNode* SourceNode = FindNodeById(Graph, SourceNodeId);
	if (!SourceNode)
	{
		OutError = FString::Printf(TEXT("Source node '%s' not found"), *SourceNodeId);
		return false;
	}

	// Find target node
	UEdGraphNode* TargetNode = FindNodeById(Graph, TargetNodeId);
	if (!TargetNode)
	{
		OutError = FString::Printf(TEXT("Target node '%s' not found"), *TargetNodeId);
		return false;
	}

	// Find pins - auto-detect exec pins if names are empty
	UEdGraphPin* SourcePin = nullptr;
	UEdGraphPin* TargetPin = nullptr;

	if (SourcePinName.IsEmpty())
	{
		// Auto-select first exec output
		SourcePin = GetExecPin(SourceNode, true);
		if (!SourcePin)
		{
			OutError = FString::Printf(TEXT("No exec output pin found on node '%s'"), *SourceNodeId);
			return false;
		}
	}
	else
	{
		SourcePin = FindPinByName(SourceNode, SourcePinName, EGPD_Output);
		if (!SourcePin)
		{
			// Try input direction for bidirectional data pins
			SourcePin = FindPinByName(SourceNode, SourcePinName, EGPD_Input);
		}
		if (!SourcePin)
		{
			OutError = FString::Printf(TEXT("Pin '%s' not found on source node '%s'"), *SourcePinName, *SourceNodeId);
			return false;
		}
	}

	if (TargetPinName.IsEmpty())
	{
		// Auto-select first exec input
		TargetPin = GetExecPin(TargetNode, false);
		if (!TargetPin)
		{
			OutError = FString::Printf(TEXT("No exec input pin found on node '%s'"), *TargetNodeId);
			return false;
		}
	}
	else
	{
		TargetPin = FindPinByName(TargetNode, TargetPinName, EGPD_Input);
		if (!TargetPin)
		{
			// Try output direction for bidirectional data pins
			TargetPin = FindPinByName(TargetNode, TargetPinName, EGPD_Output);
		}
		if (!TargetPin)
		{
			OutError = FString::Printf(TEXT("Pin '%s' not found on target node '%s'"), *TargetPinName, *TargetNodeId);
			return false;
		}
	}

	// Use schema to create connection (handles AnimGraph local/component space conversions)
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (Schema)
	{
		FPinConnectionResponse Response = Schema->CanCreateConnection(SourcePin, TargetPin);
		if (Response.Response == CONNECT_RESPONSE_DISALLOW)
		{
			OutError = FString::Printf(TEXT("Cannot connect pins: %s"), *Response.Message.ToString());
			return false;
		}

		if (!Schema->TryCreateConnection(SourcePin, TargetPin))
		{
			OutError = TEXT("Schema rejected connection");
			return false;
		}
	}
	else
	{
		SourcePin->MakeLinkTo(TargetPin);
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("Connected '%s.%s' -> '%s.%s'"),
		*SourceNodeId, *SourcePin->PinName.ToString(),
		*TargetNodeId, *TargetPin->PinName.ToString());

	return true;
}

bool FBlueprintGraphEditor::DisconnectPins(
	UEdGraph* Graph,
	const FString& SourceNodeId,
	const FString& SourcePinName,
	const FString& TargetNodeId,
	const FString& TargetPinName,
	FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return false;
	}

	// Find nodes
	UEdGraphNode* SourceNode = FindNodeById(Graph, SourceNodeId);
	if (!SourceNode)
	{
		OutError = FString::Printf(TEXT("Source node '%s' not found"), *SourceNodeId);
		return false;
	}

	UEdGraphNode* TargetNode = FindNodeById(Graph, TargetNodeId);
	if (!TargetNode)
	{
		OutError = FString::Printf(TEXT("Target node '%s' not found"), *TargetNodeId);
		return false;
	}

	// Find pins
	UEdGraphPin* SourcePin = FindPinByName(SourceNode, SourcePinName, EGPD_MAX);
	if (!SourcePin)
	{
		OutError = FString::Printf(TEXT("Pin '%s' not found on source node '%s'"), *SourcePinName, *SourceNodeId);
		return false;
	}

	UEdGraphPin* TargetPin = FindPinByName(TargetNode, TargetPinName, EGPD_MAX);
	if (!TargetPin)
	{
		OutError = FString::Printf(TEXT("Pin '%s' not found on target node '%s'"), *TargetPinName, *TargetNodeId);
		return false;
	}

	// Break the link
	SourcePin->BreakLinkTo(TargetPin);

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("Disconnected '%s.%s' from '%s.%s'"),
		*SourceNodeId, *SourcePinName,
		*TargetNodeId, *TargetPinName);

	return true;
}

bool FBlueprintGraphEditor::SetPinDefaultValue(
	UEdGraph* Graph,
	const FString& NodeId,
	const FString& PinName,
	const FString& Value,
	FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Graph is null");
		return false;
	}

	UEdGraphNode* Node = FindNodeById(Graph, NodeId);
	if (!Node)
	{
		OutError = FString::Printf(TEXT("Node '%s' not found"), *NodeId);
		return false;
	}

	UEdGraphPin* Pin = FindPinByName(Node, PinName, EGPD_Input);
	if (!Pin)
	{
		OutError = FString::Printf(TEXT("Input pin '%s' not found on node '%s'"), *PinName, *NodeId);
		return false;
	}

	// Set the default value
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (Schema)
	{
		Schema->TrySetDefaultValue(*Pin, Value);
	}
	else
	{
		Pin->DefaultValue = Value;
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("Set pin '%s.%s' default value to '%s'"), *NodeId, *PinName, *Value);
	return true;
}

UEdGraphPin* FBlueprintGraphEditor::FindPinByName(
	UEdGraphNode* Node,
	const FString& PinName,
	EEdGraphPinDirection Direction)
{
	if (!Node)
	{
		return nullptr;
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			if (Direction == EGPD_MAX || Pin->Direction == Direction)
			{
				return Pin;
			}
		}
	}

	return nullptr;
}

UEdGraphPin* FBlueprintGraphEditor::GetExecPin(UEdGraphNode* Node, bool bOutput)
{
	if (!Node)
	{
		return nullptr;
	}

	EEdGraphPinDirection Direction = bOutput ? EGPD_Output : EGPD_Input;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			return Pin;
		}
	}

	return nullptr;
}

// ===== Serialization =====

TSharedPtr<FJsonObject> FBlueprintGraphEditor::SerializeNodeInfo(UEdGraphNode* Node)
{
	TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();

	if (!Node)
	{
		return NodeObj;
	}

	NodeObj->SetStringField(TEXT("node_id"), GetNodeId(Node));
	NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
	NodeObj->SetNumberField(TEXT("pos_x"), Node->NodePosX);
	NodeObj->SetNumberField(TEXT("pos_y"), Node->NodePosY);

	// Serialize pins
	TArray<TSharedPtr<FJsonValue>> Pins;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
		{
			TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
			PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));

			// Convert pin type to string representation
			FString TypeStr;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
			{
				TypeStr = TEXT("bool");
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
			{
				TypeStr = TEXT("int32");
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
			{
				TypeStr = (Pin->PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double) ? TEXT("double") : TEXT("float");
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_String)
			{
				TypeStr = TEXT("FString");
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				TypeStr = TEXT("exec");
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				if (UScriptStruct* Struct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get()))
				{
					TypeStr = Struct->GetName();
				}
			}
			else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
			{
				if (UClass* Class = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get()))
				{
					TypeStr = Class->GetName() + TEXT("*");
				}
			}
			else
			{
				TypeStr = Pin->PinType.PinCategory.ToString();
			}

			PinObj->SetStringField(TEXT("type"), TypeStr);
			if (!Pin->DefaultValue.IsEmpty())
			{
				PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
			}
			PinObj->SetNumberField(TEXT("connections"), Pin->LinkedTo.Num());
			Pins.Add(MakeShared<FJsonValueObject>(PinObj));
		}
	}
	NodeObj->SetArrayField(TEXT("pins"), Pins);

	return NodeObj;
}

// ===== Node ID System =====

FString FBlueprintGraphEditor::GenerateNodeId(const FString& NodeType, const FString& Context, UEdGraph* Graph)
{
	FString BaseId;
	if (Context.IsEmpty())
	{
		BaseId = NodeType;
	}
	else
	{
		BaseId = FString::Printf(TEXT("%s_%s"), *NodeType, *Context);
	}

	// Ensure uniqueness with atomic increment for thread safety
	int32 Counter = FPlatformAtomics::InterlockedIncrement(&NodeIdCounter);
	FString NodeId = FString::Printf(TEXT("%s_%d"), *BaseId, Counter);

	// Verify uniqueness in graph
	if (Graph)
	{
		while (FindNodeById(Graph, NodeId) != nullptr)
		{
			Counter = FPlatformAtomics::InterlockedIncrement(&NodeIdCounter);
			NodeId = FString::Printf(TEXT("%s_%d"), *BaseId, Counter);
		}
	}

	return NodeId;
}

void FBlueprintGraphEditor::SetNodeId(UEdGraphNode* Node, const FString& NodeId)
{
	if (Node)
	{
		// Store ID in node comment (visible in editor, persisted)
		Node->NodeComment = NodeIdPrefix + NodeId;
	}
}

FString FBlueprintGraphEditor::GetNodeId(UEdGraphNode* Node)
{
	if (!Node)
	{
		return FString();
	}

	// Extract ID from node comment
	if (Node->NodeComment.StartsWith(NodeIdPrefix))
	{
		return Node->NodeComment.RightChop(NodeIdPrefix.Len());
	}

	return FString();
}

// ===== Private Node Creation Helpers =====

UEdGraphNode* FBlueprintGraphEditor::CreateCallFunctionNode(
	UEdGraph* Graph,
	const FString& FunctionName,
	const FString& TargetClass,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (FunctionName.IsEmpty())
	{
		OutError = TEXT("Function name is required");
		return nullptr;
	}

	// Find the function
	UFunction* Function = nullptr;
	UClass* FunctionOwner = nullptr;

	// Try to find class by name
	if (!TargetClass.IsEmpty())
	{
		// FindFirstObject (UE 5.1+) replaces deprecated ANY_PACKAGE search, with NativeFirst
		// to prefer C++ classes over BP-generated when names collide.
		FunctionOwner = FindFirstObject<UClass>(*TargetClass, EFindFirstObjectOptions::NativeFirst);

		// Retry with engine prefix if user dropped it (e.g. "PrimitiveComponent" -> "UPrimitiveComponent")
		if (!FunctionOwner && !TargetClass.StartsWith(TEXT("U")) && !TargetClass.StartsWith(TEXT("A")))
		{
			const FString WithU = FString(TEXT("U")) + TargetClass;
			FunctionOwner = FindFirstObject<UClass>(*WithU, EFindFirstObjectOptions::NativeFirst);
			if (!FunctionOwner)
			{
				const FString WithA = FString(TEXT("A")) + TargetClass;
				FunctionOwner = FindFirstObject<UClass>(*WithA, EFindFirstObjectOptions::NativeFirst);
			}
		}

		if (!FunctionOwner)
		{
			// Bare-name fallback for the common kismet libraries (mirrors prior behaviour)
			if (TargetClass.Equals(TEXT("KismetSystemLibrary"), ESearchCase::IgnoreCase))
			{
				FunctionOwner = UKismetSystemLibrary::StaticClass();
			}
			else if (TargetClass.Equals(TEXT("KismetMathLibrary"), ESearchCase::IgnoreCase))
			{
				FunctionOwner = UKismetMathLibrary::StaticClass();
			}
			else if (TargetClass.Equals(TEXT("KismetStringLibrary"), ESearchCase::IgnoreCase))
			{
				FunctionOwner = UKismetStringLibrary::StaticClass();
			}
			else if (TargetClass.Equals(TEXT("AnimInstance"), ESearchCase::IgnoreCase))
			{
				FunctionOwner = UAnimInstance::StaticClass();
			}
			else if (TargetClass.Equals(TEXT("GameplayStatics"), ESearchCase::IgnoreCase))
			{
				FunctionOwner = UGameplayStatics::StaticClass();
			}
		}
	}
	else
	{
		// Default to KismetSystemLibrary for common functions
		FunctionOwner = UKismetSystemLibrary::StaticClass();
	}

	if (FunctionOwner)
	{
		Function = FunctionOwner->FindFunctionByName(FName(*FunctionName));
	}

	// If not found in specified class, search common libraries
	if (!Function)
	{
		Function = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FName(*FunctionName));
	}
	if (!Function)
	{
		Function = UKismetMathLibrary::StaticClass()->FindFunctionByName(FName(*FunctionName));
	}
	if (!Function)
	{
		Function = UKismetStringLibrary::StaticClass()->FindFunctionByName(FName(*FunctionName));
	}
	if (!Function)
	{
		Function = UGameplayStatics::StaticClass()->FindFunctionByName(FName(*FunctionName));
	}

	if (!Function)
	{
		OutError = FString::Printf(TEXT("Function '%s' not found"), *FunctionName);
		return nullptr;
	}

	// Create the node
	FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*Graph);
	UK2Node_CallFunction* CallNode = NodeCreator.CreateNode();
	CallNode->SetFromFunction(Function);
	CallNode->NodePosX = PosX;
	CallNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return CallNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateBranchNode(
	UEdGraph* Graph,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	FGraphNodeCreator<UK2Node_IfThenElse> NodeCreator(*Graph);
	UK2Node_IfThenElse* BranchNode = NodeCreator.CreateNode();
	BranchNode->NodePosX = PosX;
	BranchNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return BranchNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateEventNode(
	UEdGraph* Graph,
	const FString& EventName,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (EventName.IsEmpty())
	{
		OutError = TEXT("Event name is required");
		return nullptr;
	}

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (!Blueprint)
	{
		OutError = TEXT("Could not find Blueprint for graph");
		return nullptr;
	}

	// Find the event function
	UFunction* EventFunc = nullptr;

	// Check common events
	if (EventName.Equals(TEXT("BeginPlay"), ESearchCase::IgnoreCase))
	{
		EventFunc = AActor::StaticClass()->FindFunctionByName(FName("ReceiveBeginPlay"));
	}
	else if (EventName.Equals(TEXT("Tick"), ESearchCase::IgnoreCase))
	{
		EventFunc = AActor::StaticClass()->FindFunctionByName(FName("ReceiveTick"));
	}
	else if (EventName.Equals(TEXT("EndPlay"), ESearchCase::IgnoreCase))
	{
		EventFunc = AActor::StaticClass()->FindFunctionByName(FName("ReceiveEndPlay"));
	}
	else
	{
		// Try to find in parent class
		if (Blueprint->ParentClass)
		{
			EventFunc = Blueprint->ParentClass->FindFunctionByName(FName(*EventName));
		}
	}

	if (!EventFunc)
	{
		OutError = FString::Printf(TEXT("Event '%s' not found. Common events: BeginPlay, Tick, EndPlay"), *EventName);
		return nullptr;
	}

	// Create the event node
	FGraphNodeCreator<UK2Node_Event> NodeCreator(*Graph);
	UK2Node_Event* EventNode = NodeCreator.CreateNode();
	EventNode->EventReference.SetFromField<UFunction>(EventFunc, false);
	EventNode->bOverrideFunction = true;
	EventNode->NodePosX = PosX;
	EventNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return EventNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateVariableGetNode(
	UEdGraph* Graph,
	UBlueprint* Blueprint,
	const FString& VariableName,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (VariableName.IsEmpty())
	{
		OutError = TEXT("Variable name is required");
		return nullptr;
	}

	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return nullptr;
	}

	// Verify variable exists (check BP variables first, then parent C++ class)
	FName VarName(*VariableName);
	bool bFound = false;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarName)
		{
			bFound = true;
			break;
		}
	}

	if (!bFound && Blueprint->ParentClass)
	{
		bFound = (Blueprint->ParentClass->FindPropertyByName(VarName) != nullptr);
	}

	// SCS components (added via add_component) live on the Skeleton class, not ParentClass
	if (!bFound && Blueprint->SkeletonGeneratedClass)
	{
		bFound = (Blueprint->SkeletonGeneratedClass->FindPropertyByName(VarName) != nullptr);
	}

	if (!bFound)
	{
		OutError = FString::Printf(TEXT("Variable '%s' not found in Blueprint, parent class, or components"), *VariableName);
		return nullptr;
	}

	// Create the node
	FGraphNodeCreator<UK2Node_VariableGet> NodeCreator(*Graph);
	UK2Node_VariableGet* GetNode = NodeCreator.CreateNode();
	GetNode->VariableReference.SetSelfMember(VarName);
	GetNode->NodePosX = PosX;
	GetNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return GetNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateVariableSetNode(
	UEdGraph* Graph,
	UBlueprint* Blueprint,
	const FString& VariableName,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (VariableName.IsEmpty())
	{
		OutError = TEXT("Variable name is required");
		return nullptr;
	}

	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return nullptr;
	}

	// Verify variable exists
	FName VarName(*VariableName);
	bool bFound = false;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarName)
		{
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		OutError = FString::Printf(TEXT("Variable '%s' not found in Blueprint"), *VariableName);
		return nullptr;
	}

	// Create the node
	FGraphNodeCreator<UK2Node_VariableSet> NodeCreator(*Graph);
	UK2Node_VariableSet* SetNode = NodeCreator.CreateNode();
	SetNode->VariableReference.SetSelfMember(VarName);
	SetNode->NodePosX = PosX;
	SetNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return SetNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateSequenceNode(
	UEdGraph* Graph,
	int32 NumOutputs,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	FGraphNodeCreator<UK2Node_ExecutionSequence> NodeCreator(*Graph);
	UK2Node_ExecutionSequence* SeqNode = NodeCreator.CreateNode();
	SeqNode->NodePosX = PosX;
	SeqNode->NodePosY = PosY;
	NodeCreator.Finalize();

	// Add additional output pins if needed (default is 2)
	while (SeqNode->Pins.Num() < NumOutputs + 1) // +1 for input exec
	{
		SeqNode->AddInputPin();
	}

	return SeqNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateMathNode(
	UEdGraph* Graph,
	const FString& MathOp,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	// Find the appropriate math function
	FName FunctionName;
	if (MathOp.Equals(TEXT("Add"), ESearchCase::IgnoreCase))
	{
		FunctionName = FName("Add_FloatFloat");
	}
	else if (MathOp.Equals(TEXT("Subtract"), ESearchCase::IgnoreCase))
	{
		FunctionName = FName("Subtract_FloatFloat");
	}
	else if (MathOp.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase))
	{
		FunctionName = FName("Multiply_FloatFloat");
	}
	else if (MathOp.Equals(TEXT("Divide"), ESearchCase::IgnoreCase))
	{
		FunctionName = FName("Divide_FloatFloat");
	}
	else
	{
		OutError = FString::Printf(TEXT("Unknown math operation: '%s'. Supported: Add, Subtract, Multiply, Divide"), *MathOp);
		return nullptr;
	}

	UFunction* MathFunc = UKismetMathLibrary::StaticClass()->FindFunctionByName(FunctionName);
	if (!MathFunc)
	{
		OutError = FString::Printf(TEXT("Math function '%s' not found"), *FunctionName.ToString());
		return nullptr;
	}

	// Create a call function node for the math operation
	FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*Graph);
	UK2Node_CallFunction* MathNode = NodeCreator.CreateNode();
	MathNode->SetFromFunction(MathFunc);
	MathNode->NodePosX = PosX;
	MathNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return MathNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateModifyBoneNode(
	UEdGraph* Graph,
	const FString& BoneName,
	const TSharedPtr<FJsonObject>& Params,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (BoneName.IsEmpty())
	{
		OutError = TEXT("bone_name is required for ModifyBone");
		return nullptr;
	}

	FGraphNodeCreator<UAnimGraphNode_ModifyBone> NodeCreator(*Graph);
	UAnimGraphNode_ModifyBone* ModifyBoneNode = NodeCreator.CreateNode();
	ModifyBoneNode->Node.BoneToModify.BoneName = FName(*BoneName);
	ModifyBoneNode->Node.TranslationMode = BMM_Additive;
	ModifyBoneNode->Node.RotationMode = BMM_Ignore;
	ModifyBoneNode->Node.ScaleMode = BMM_Ignore;
	ModifyBoneNode->Node.TranslationSpace = BCS_ComponentSpace;

	if (Params.IsValid())
	{
		FString TransMode = Params->GetStringField(TEXT("translation_mode"));
		if (TransMode.Equals(TEXT("Replace"), ESearchCase::IgnoreCase))
			ModifyBoneNode->Node.TranslationMode = BMM_Replace;
		else if (TransMode.Equals(TEXT("Ignore"), ESearchCase::IgnoreCase))
			ModifyBoneNode->Node.TranslationMode = BMM_Ignore;

		FString TransSpace = Params->GetStringField(TEXT("translation_space"));
		if (TransSpace.Equals(TEXT("WorldSpace"), ESearchCase::IgnoreCase))
			ModifyBoneNode->Node.TranslationSpace = BCS_WorldSpace;
		else if (TransSpace.Equals(TEXT("BoneSpace"), ESearchCase::IgnoreCase))
			ModifyBoneNode->Node.TranslationSpace = BCS_BoneSpace;
		else if (TransSpace.Equals(TEXT("ParentBoneSpace"), ESearchCase::IgnoreCase))
			ModifyBoneNode->Node.TranslationSpace = BCS_ParentBoneSpace;
	}

	ModifyBoneNode->NodePosX = PosX;
	ModifyBoneNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return ModifyBoneNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateTwoBoneIKNode(
	UEdGraph* Graph,
	const FString& BoneName,
	const TSharedPtr<FJsonObject>& Params,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (BoneName.IsEmpty())
	{
		OutError = TEXT("bone_name is required for TwoBoneIK");
		return nullptr;
	}

	FGraphNodeCreator<UAnimGraphNode_TwoBoneIK> NodeCreator(*Graph);
	UAnimGraphNode_TwoBoneIK* IKNode = NodeCreator.CreateNode();
	IKNode->Node.IKBone.BoneName = FName(*BoneName);
	IKNode->Node.EffectorLocationSpace = BCS_BoneSpace;

	if (Params.IsValid())
	{
		FString EffSpace = Params->GetStringField(TEXT("effector_space"));
		if (EffSpace.Equals(TEXT("ComponentSpace"), ESearchCase::IgnoreCase))
			IKNode->Node.EffectorLocationSpace = BCS_ComponentSpace;
		else if (EffSpace.Equals(TEXT("WorldSpace"), ESearchCase::IgnoreCase))
			IKNode->Node.EffectorLocationSpace = BCS_WorldSpace;
		else if (EffSpace.Equals(TEXT("ParentBoneSpace"), ESearchCase::IgnoreCase))
			IKNode->Node.EffectorLocationSpace = BCS_ParentBoneSpace;
	}

	IKNode->NodePosX = PosX;
	IKNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return IKNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateEnhancedInputActionNode(
	UEdGraph* Graph,
	const FString& ActionPath,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (ActionPath.IsEmpty())
	{
		OutError = TEXT("action_path is required for EnhancedInputAction (e.g. '/Game/Input/IA_Look')");
		return nullptr;
	}

	// Load the UInputAction asset
	UInputAction* InputAction = LoadObject<UInputAction>(nullptr, *ActionPath);
	if (!InputAction)
	{
		OutError = FString::Printf(TEXT("InputAction '%s' not found"), *ActionPath);
		return nullptr;
	}

	// Create the node
	FGraphNodeCreator<UK2Node_EnhancedInputAction> NodeCreator(*Graph);
	UK2Node_EnhancedInputAction* ActionNode = NodeCreator.CreateNode();
	ActionNode->InputAction = InputAction;
	ActionNode->NodePosX = PosX;
	ActionNode->NodePosY = PosY;
	NodeCreator.Finalize();

	// AllocateDefaultPins is called by Finalize, which creates:
	// - Exec output pins for each ETriggerEvent (Started, Ongoing, Triggered, Completed, Canceled)
	// - ActionValue output pin (type matches InputAction ValueType)
	// - ElapsedSeconds, TriggeredSeconds output pins
	// - InputAction output pin
	// "Triggered" is the most commonly used exec pin.

	return ActionNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateControlRigNode(
	UEdGraph* Graph,
	const FString& ControlRigClass,
	const TSharedPtr<FJsonObject>& Params,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (ControlRigClass.IsEmpty())
	{
		OutError = TEXT("control_rig_class is required for ControlRig (e.g. '/Game/Blueprints/CR_MyControlRig')");
		return nullptr;
	}

	// Load the Control Rig Blueprint asset (load as UBlueprint — works for all BP types)
	UBlueprint* RigBP = LoadObject<UBlueprint>(nullptr, *ControlRigClass);
	if (!RigBP)
	{
		OutError = FString::Printf(TEXT("Control Rig Blueprint '%s' not found"), *ControlRigClass);
		return nullptr;
	}

	// Get the generated class and verify it's a UControlRig subclass
	UClass* RigClass = RigBP->GeneratedClass;
	if (!RigClass)
	{
		OutError = FString::Printf(TEXT("Control Rig '%s' has no generated class (compile it first)"), *ControlRigClass);
		return nullptr;
	}

	if (!RigClass->IsChildOf(UControlRig::StaticClass()))
	{
		OutError = FString::Printf(TEXT("'%s' is not a Control Rig (generated class is not a UControlRig subclass)"), *ControlRigClass);
		return nullptr;
	}

	FGraphNodeCreator<UAnimGraphNode_ControlRig> NodeCreator(*Graph);
	UAnimGraphNode_ControlRig* CRNode = NodeCreator.CreateNode();
	CRNode->Node.SetControlRigClass(RigClass);
	CRNode->NodePosX = PosX;
	CRNode->NodePosY = PosY;
	NodeCreator.Finalize();

	// Auto-expose all Control Rig variable pins via UE reflection
	// (CustomPinProperties is protected in UAnimGraphNode_CustomProperty)
	FProperty* Prop = UAnimGraphNode_CustomProperty::StaticClass()->FindPropertyByName(TEXT("CustomPinProperties"));
	FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
	if (ArrayProp)
	{
		TArray<FOptionalPinFromProperty>* PinProps =
			ArrayProp->ContainerPtrToValuePtr<TArray<FOptionalPinFromProperty>>(CRNode);
		if (PinProps)
		{
			for (FOptionalPinFromProperty& PinProp : *PinProps)
			{
				if (PinProp.bCanToggleVisibility)
				{
					PinProp.bShowPin = true;
				}
			}
		}
	}
	CRNode->ReconstructNode();

	return CRNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateCustomEventNode(
	UEdGraph* Graph,
	const FString& EventName,
	const TSharedPtr<FJsonObject>& Params,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (EventName.IsEmpty())
	{
		OutError = TEXT("event_name is required for CustomEvent");
		return nullptr;
	}

	FGraphNodeCreator<UK2Node_CustomEvent> NodeCreator(*Graph);
	UK2Node_CustomEvent* CustomEventNode = NodeCreator.CreateNode();
	CustomEventNode->CustomFunctionName = FName(*EventName);
	CustomEventNode->NodePosX = PosX;
	CustomEventNode->NodePosY = PosY;
	NodeCreator.Finalize();

	// Optional user-defined input pins from {"inputs":[{"name":"X","type":"float"}, ...]}
	if (Params.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
		if (Params->TryGetArrayField(TEXT("inputs"), Inputs))
		{
			for (const TSharedPtr<FJsonValue>& InputVal : *Inputs)
			{
				const TSharedPtr<FJsonObject>* InputObj = nullptr;
				if (!InputVal.IsValid() || !InputVal->TryGetObject(InputObj) || !InputObj) continue;

				const FString InputName = (*InputObj)->GetStringField(TEXT("name"));
				const FString InputType = (*InputObj)->GetStringField(TEXT("type"));
				if (InputName.IsEmpty() || InputType.IsEmpty()) continue;

				FEdGraphPinType PinType;
				if (InputType.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
				}
				else if (InputType.Equals(TEXT("int32"), ESearchCase::IgnoreCase) || InputType.Equals(TEXT("int"), ESearchCase::IgnoreCase))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
				}
				else if (InputType.Equals(TEXT("float"), ESearchCase::IgnoreCase))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
					PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
				}
				else if (InputType.Equals(TEXT("double"), ESearchCase::IgnoreCase))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
					PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
				}
				else if (InputType.Equals(TEXT("FString"), ESearchCase::IgnoreCase) || InputType.Equals(TEXT("string"), ESearchCase::IgnoreCase))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_String;
				}
				else if (InputType.Equals(TEXT("FName"), ESearchCase::IgnoreCase) || InputType.Equals(TEXT("name"), ESearchCase::IgnoreCase))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
				}
				else if (InputType.Equals(TEXT("FVector"), ESearchCase::IgnoreCase) || InputType.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
					PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
				}
				else if (InputType.Equals(TEXT("FRotator"), ESearchCase::IgnoreCase) || InputType.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
					PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
				}
				else
				{
					UE_LOG(LogUnrealClaude, Warning, TEXT("CustomEvent input '%s': unsupported type '%s', skipping"), *InputName, *InputType);
					continue;
				}

				CustomEventNode->CreateUserDefinedPin(FName(*InputName), PinType, EGPD_Output);
			}
		}
	}

	return CustomEventNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateComponentBoundEventNode(
	UEdGraph* Graph,
	UBlueprint* Blueprint,
	const FString& ComponentName,
	const FString& DelegateName,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (ComponentName.IsEmpty())
	{
		OutError = TEXT("component is required for ComponentBoundEvent (SCS variable name, e.g. 'Cube')");
		return nullptr;
	}
	if (DelegateName.IsEmpty())
	{
		OutError = TEXT("delegate is required for ComponentBoundEvent (e.g. 'OnComponentHit', 'OnComponentBeginOverlap')");
		return nullptr;
	}

	UClass* SkeletonClass = Blueprint ? Blueprint->SkeletonGeneratedClass : nullptr;
	if (!SkeletonClass)
	{
		OutError = TEXT("Blueprint has no SkeletonGeneratedClass (try compiling first)");
		return nullptr;
	}

	FObjectProperty* CompProp = CastField<FObjectProperty>(SkeletonClass->FindPropertyByName(FName(*ComponentName)));
	if (!CompProp)
	{
		OutError = FString::Printf(TEXT("Component '%s' not found on Blueprint"), *ComponentName);
		return nullptr;
	}

	UClass* CompClass = CompProp->PropertyClass;
	if (!CompClass)
	{
		OutError = FString::Printf(TEXT("Component '%s' has no class"), *ComponentName);
		return nullptr;
	}

	FMulticastDelegateProperty* DelegateProp = CastField<FMulticastDelegateProperty>(
		CompClass->FindPropertyByName(FName(*DelegateName))
	);
	if (!DelegateProp)
	{
		OutError = FString::Printf(TEXT("Delegate '%s' not found on component class '%s'"), *DelegateName, *CompClass->GetName());
		return nullptr;
	}

	FGraphNodeCreator<UK2Node_ComponentBoundEvent> NodeCreator(*Graph);
	UK2Node_ComponentBoundEvent* EventNode = NodeCreator.CreateNode();
	EventNode->InitializeComponentBoundEventParams(CompProp, DelegateProp);
	EventNode->NodePosX = PosX;
	EventNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return EventNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateSelfNode(
	UEdGraph* Graph,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	FGraphNodeCreator<UK2Node_Self> NodeCreator(*Graph);
	UK2Node_Self* SelfNode = NodeCreator.CreateNode();
	SelfNode->NodePosX = PosX;
	SelfNode->NodePosY = PosY;
	NodeCreator.Finalize();
	return SelfNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateCastNode(
	UEdGraph* Graph,
	const FString& TargetClass,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (TargetClass.IsEmpty())
	{
		OutError = TEXT("target_class is required for Cast (e.g. 'Character', 'StaticMeshActor')");
		return nullptr;
	}

	UClass* CastClass = FindFirstObject<UClass>(*TargetClass, EFindFirstObjectOptions::NativeFirst);
	if (!CastClass && !TargetClass.StartsWith(TEXT("U")) && !TargetClass.StartsWith(TEXT("A")))
	{
		const FString WithU = FString(TEXT("U")) + TargetClass;
		CastClass = FindFirstObject<UClass>(*WithU, EFindFirstObjectOptions::NativeFirst);
		if (!CastClass)
		{
			const FString WithA = FString(TEXT("A")) + TargetClass;
			CastClass = FindFirstObject<UClass>(*WithA, EFindFirstObjectOptions::NativeFirst);
		}
	}
	if (!CastClass)
	{
		OutError = FString::Printf(TEXT("Class '%s' not found for Cast"), *TargetClass);
		return nullptr;
	}

	FGraphNodeCreator<UK2Node_DynamicCast> NodeCreator(*Graph);
	UK2Node_DynamicCast* CastNode = NodeCreator.CreateNode();
	CastNode->TargetType = CastClass;
	CastNode->NodePosX = PosX;
	CastNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return CastNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateMakeStructNode(
	UEdGraph* Graph,
	const FString& StructType,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (StructType.IsEmpty())
	{
		OutError = TEXT("struct_type is required for MakeStruct (e.g. 'Vector', 'LinearColor', 'HitResult')");
		return nullptr;
	}

	UScriptStruct* Struct = FindFirstObject<UScriptStruct>(*StructType, EFindFirstObjectOptions::NativeFirst);
	if (!Struct && !StructType.StartsWith(TEXT("F")))
	{
		const FString WithF = FString(TEXT("F")) + StructType;
		Struct = FindFirstObject<UScriptStruct>(*WithF, EFindFirstObjectOptions::NativeFirst);
	}
	if (!Struct)
	{
		OutError = FString::Printf(TEXT("Struct '%s' not found for MakeStruct"), *StructType);
		return nullptr;
	}

	FGraphNodeCreator<UK2Node_MakeStruct> NodeCreator(*Graph);
	UK2Node_MakeStruct* MakeNode = NodeCreator.CreateNode();
	MakeNode->StructType = Struct;
	MakeNode->NodePosX = PosX;
	MakeNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return MakeNode;
}

UEdGraphNode* FBlueprintGraphEditor::CreateBreakStructNode(
	UEdGraph* Graph,
	const FString& StructType,
	int32 PosX,
	int32 PosY,
	FString& OutError)
{
	if (StructType.IsEmpty())
	{
		OutError = TEXT("struct_type is required for BreakStruct (e.g. 'HitResult', 'Vector', 'Transform')");
		return nullptr;
	}

	UScriptStruct* Struct = FindFirstObject<UScriptStruct>(*StructType, EFindFirstObjectOptions::NativeFirst);
	if (!Struct && !StructType.StartsWith(TEXT("F")))
	{
		const FString WithF = FString(TEXT("F")) + StructType;
		Struct = FindFirstObject<UScriptStruct>(*WithF, EFindFirstObjectOptions::NativeFirst);
	}
	if (!Struct)
	{
		OutError = FString::Printf(TEXT("Struct '%s' not found for BreakStruct"), *StructType);
		return nullptr;
	}

	FGraphNodeCreator<UK2Node_BreakStruct> NodeCreator(*Graph);
	UK2Node_BreakStruct* BreakNode = NodeCreator.CreateNode();
	BreakNode->StructType = Struct;
	BreakNode->NodePosX = PosX;
	BreakNode->NodePosY = PosY;
	NodeCreator.Finalize();

	return BreakNode;
}
