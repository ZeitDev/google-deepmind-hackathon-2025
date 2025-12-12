#include "NodeDumperLibrary.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "UObject/UObjectIterator.h" 
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/Blueprint.h"       
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Misc/Paths.h"

// NODE TYPES
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h" 
#include "K2Node_ExecutionSequence.h" 
#include "K2Node_DynamicCast.h" 
#include "EdGraphSchema_K2.h" // Crucial for Pin Type conversion

void UNodeDumperLibrary::DumpAllNodes(FString BaseFilePath)
{
    // Create a temporary graph to instantiate nodes safely
    UBlueprint* TempBP = NewObject<UBlueprint>(GetTransientPackage(), FName("TempBP"));
    UEdGraph* TempGraph = NewObject<UEdGraph>(TempBP, FName("TempGraph"));
    TempGraph->Schema = UEdGraphSchema_K2::StaticClass();
    TempBP->FunctionGraphs.Add(TempGraph);

    // --- DATA CONTAINERS ---
    TArray<TSharedPtr<FJsonValue>> FullArray;
    TArray<TSharedPtr<FJsonValue>> EssentialsArray;
    TArray<TSharedPtr<FJsonValue>> DebugArray;

    // --- HELPERS ---
    auto GetT3DPath = [](UObject* Obj) -> FString {
        if (!Obj) return "None";
        FString Path = Obj->GetPathName();
        if (Obj->IsA<UScriptStruct>()) return FString::Printf(TEXT("/Script/CoreUObject.ScriptStruct'%s'"), *Path);
        if (Obj->IsA<UClass>()) return FString::Printf(TEXT("/Script/CoreUObject.Class'%s'"), *Path);
        if (Obj->IsA<UEnum>()) return FString::Printf(TEXT("/Script/CoreUObject.Enum'%s'"), *Path);
        return Path;
        };

    auto GetContainerTypeString = [](EPinContainerType ContainerType) -> FString {
        switch (ContainerType) {
        case EPinContainerType::Array: return "Array";
        case EPinContainerType::Set:   return "Set";
        case EPinContainerType::Map:   return "Map";
        default:                       return "None";
        }
        };

    // --- FUNCTION DUMPING LAMBDA ---
    auto DumpNodeToJSON = [&](UK2Node* Node, FString NodeTypeOverride, FString MemberParent, FString FuncName) {
        Node->AllocateDefaultPins();

        TSharedPtr<FJsonObject> NodeObj = MakeShareable(new FJsonObject);

        FString Name = FuncName;
        if (Name.IsEmpty()) Name = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

        // Standardize Name
        FString CleanName = Name.Replace(TEXT(" "), TEXT(""));

        NodeObj->SetStringField("Name", Name);
        NodeObj->SetStringField("NodeType", NodeTypeOverride.IsEmpty() ? Node->GetClass()->GetName() : NodeTypeOverride);
        NodeObj->SetStringField("FunctionName", FuncName);
        NodeObj->SetStringField("MemberParent", MemberParent);

        // Metadata
        FString Keywords = "";
        FString ToolTip = "";
        if (UK2Node_CallFunction* CallFuncNode = Cast<UK2Node_CallFunction>(Node)) {
            if (UFunction* Func = CallFuncNode->GetTargetFunction()) {
                Keywords = Func->GetMetaData(TEXT("Keywords"));
                ToolTip = Func->GetToolTipText().ToString();
            }
        }
        NodeObj->SetStringField("Keywords", Keywords);
        NodeObj->SetStringField("ToolTip", ToolTip);

        // Pins
        TArray<TSharedPtr<FJsonValue>> Inputs;
        TArray<TSharedPtr<FJsonValue>> Outputs;

        for (UEdGraphPin* Pin : Node->Pins) {
            TSharedPtr<FJsonObject> PinObj = MakeShareable(new FJsonObject);
            PinObj->SetStringField("PinName", Pin->PinName.ToString());
            PinObj->SetStringField("PinCategory", Pin->PinType.PinCategory.ToString());
            PinObj->SetStringField("PinSubCategory", Pin->PinType.PinSubCategory.ToString());
            PinObj->SetStringField("PinSubCategoryObject", GetT3DPath(Pin->PinType.PinSubCategoryObject.Get()));
            PinObj->SetStringField("ContainerType", GetContainerTypeString(Pin->PinType.ContainerType));
            PinObj->SetBoolField("bIsReference", Pin->PinType.bIsReference);
            PinObj->SetBoolField("bIsConst", Pin->PinType.bIsConst);

            bool bHidden = Pin->bHidden;
            if (Pin->PinName.ToString() == "WorldContextObject") bHidden = true;
            if (Pin->PinName.ToString() == "self") bHidden = true;

            PinObj->SetBoolField("bHidden", bHidden);
            PinObj->SetStringField("DefaultValue", Pin->DefaultValue);

            if (Pin->Direction == EGPD_Input) Inputs.Add(MakeShareable(new FJsonValueObject(PinObj)));
            else Outputs.Add(MakeShareable(new FJsonValueObject(PinObj)));
        }

        NodeObj->SetArrayField("Inputs", Inputs);
        NodeObj->SetArrayField("Outputs", Outputs);

        TSharedPtr<FJsonValue> Val = MakeShareable(new FJsonValueObject(NodeObj));

        // --- SORTING ---
        FullArray.Add(Val);

        bool bIsDebug = CleanName.Contains("PrintString") || CleanName.Contains("DrawDebug");
        if (bIsDebug) DebugArray.Add(Val);

        bool bIsEssential = false;
        if (NodeTypeOverride.Contains("IfThenElse") || NodeTypeOverride.Contains("ExecutionSequence")) bIsEssential = true;
        if (FuncName.Equals("Delay") || FuncName.Equals("RetriggerableDelay") || FuncName.Equals("IsValid")) bIsEssential = true;

        // KismetMathLibrary Logic
        if (MemberParent.Contains("KismetMathLibrary")) {
            // Broad filter for math operations
            if (FuncName.Contains("Add") || FuncName.Contains("Subtract") || FuncName.Contains("Multiply") ||
                FuncName.Contains("Divide") || FuncName.Contains("Equal") || FuncName.Contains("Less") ||
                FuncName.Contains("Greater") || FuncName.Contains("Boolean") || FuncName.Contains("Vector")) {
                bIsEssential = true;
            }
        }

        if (bIsEssential) EssentialsArray.Add(Val);

        Node->DestroyNode();
        };

    // ----------------------------------------------------------------
    // 1. ITERATE CLASSES & FUNCTIONS
    // ----------------------------------------------------------------
    for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
    {
        UClass* CurrentClass = *ClassIt;
        if (CurrentClass->GetName().StartsWith("SKEL_") || CurrentClass->GetName().StartsWith("REINST_")) continue;

        // Functions
        for (TFieldIterator<UFunction> FuncIt(CurrentClass); FuncIt; ++FuncIt)
        {
            UFunction* Func = *FuncIt;
            if (Func->GetOuter() != CurrentClass) continue;

            if (Func->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure)) {
                UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(TempGraph);
                Node->SetFromFunction(Func);
                DumpNodeToJSON(Node, "K2Node_CallFunction", GetT3DPath(CurrentClass), Func->GetName());
            }
            else if (Func->HasAnyFunctionFlags(FUNC_BlueprintEvent) && Func->GetName().Find("ExecuteUbergraph") == -1) {
                UK2Node_Event* EventNode = NewObject<UK2Node_Event>(TempGraph);
                EventNode->EventReference.SetExternalMember(Func->GetFName(), CurrentClass);
                DumpNodeToJSON(EventNode, "K2Node_Event", GetT3DPath(CurrentClass), Func->GetName());
            }
        }

        // ----------------------------------------------------------------
        // 2. ITERATE PROPERTIES (VARIABLES) - GETTERS & SETTERS
        // ----------------------------------------------------------------
        for (TFieldIterator<FProperty> PropIt(CurrentClass, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (!Prop->HasAnyPropertyFlags(CPF_BlueprintVisible)) continue;
            if (Prop->HasAnyPropertyFlags(CPF_Deprecated)) continue; // Skip deprecated

            FString VarName = Prop->GetName();
            FString MemberPath = GetT3DPath(CurrentClass);

            // Resolve Pin Type once
            FEdGraphPinType PinType;
            const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
            K2Schema->ConvertPropertyToPinType(Prop, PinType);

            // ---------------------
            // A) GENERATE GETTER
            // ---------------------
            {
                TSharedPtr<FJsonObject> GetNode = MakeShareable(new FJsonObject);
                GetNode->SetStringField("Name", "Get " + VarName);
                GetNode->SetStringField("NodeType", "K2Node_VariableGet");
                GetNode->SetStringField("FunctionName", VarName);
                GetNode->SetStringField("MemberParent", MemberPath);
                GetNode->SetStringField("Keywords", Prop->GetMetaData(TEXT("Keywords")));
                GetNode->SetStringField("ToolTip", "Get " + VarName);

                TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;

                // Self Pin
                TSharedPtr<FJsonObject> SelfPin = MakeShareable(new FJsonObject);
                SelfPin->SetStringField("PinName", "self");
                SelfPin->SetStringField("PinCategory", "object");
                SelfPin->SetStringField("PinSubCategory", "self");
                SelfPin->SetStringField("PinSubCategoryObject", MemberPath);
                SelfPin->SetBoolField("bIsHidden", false);
                Inputs.Add(MakeShareable(new FJsonValueObject(SelfPin)));

                // Output Value Pin
                TSharedPtr<FJsonObject> OutPin = MakeShareable(new FJsonObject);
                OutPin->SetStringField("PinName", VarName);
                OutPin->SetStringField("PinCategory", PinType.PinCategory.ToString());
                OutPin->SetStringField("PinSubCategory", PinType.PinSubCategory.ToString());
                OutPin->SetStringField("PinSubCategoryObject", GetT3DPath(PinType.PinSubCategoryObject.Get()));
                OutPin->SetStringField("ContainerType", GetContainerTypeString(PinType.ContainerType));
                OutPin->SetBoolField("bIsReference", PinType.bIsReference);
                OutPin->SetBoolField("bIsConst", true);
                Outputs.Add(MakeShareable(new FJsonValueObject(OutPin)));

                GetNode->SetArrayField("Inputs", Inputs);
                GetNode->SetArrayField("Outputs", Outputs);
                FullArray.Add(MakeShareable(new FJsonValueObject(GetNode)));
            }

            // ---------------------
            // B) GENERATE SETTER (If not ReadOnly)
            // ---------------------
            if (!Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
            {
                TSharedPtr<FJsonObject> SetNode = MakeShareable(new FJsonObject);
                SetNode->SetStringField("Name", "Set " + VarName);
                SetNode->SetStringField("NodeType", "K2Node_VariableSet");
                SetNode->SetStringField("FunctionName", VarName);
                SetNode->SetStringField("MemberParent", MemberPath);
                SetNode->SetStringField("Keywords", Prop->GetMetaData(TEXT("Keywords")));
                SetNode->SetStringField("ToolTip", "Set " + VarName);

                TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;

                // Input: Exec
                TSharedPtr<FJsonObject> ExecIn = MakeShareable(new FJsonObject);
                ExecIn->SetStringField("PinName", "execute");
                ExecIn->SetStringField("PinCategory", "exec");
                Inputs.Add(MakeShareable(new FJsonValueObject(ExecIn)));

                // Input: Self
                TSharedPtr<FJsonObject> SelfPin = MakeShareable(new FJsonObject);
                SelfPin->SetStringField("PinName", "self");
                SelfPin->SetStringField("PinCategory", "object");
                SelfPin->SetStringField("PinSubCategory", "self");
                SelfPin->SetStringField("PinSubCategoryObject", MemberPath);
                SelfPin->SetBoolField("bIsHidden", false);
                Inputs.Add(MakeShareable(new FJsonValueObject(SelfPin)));

                // Input: Value
                TSharedPtr<FJsonObject> ValIn = MakeShareable(new FJsonObject);
                ValIn->SetStringField("PinName", VarName);
                ValIn->SetStringField("PinCategory", PinType.PinCategory.ToString());
                ValIn->SetStringField("PinSubCategory", PinType.PinSubCategory.ToString());
                ValIn->SetStringField("PinSubCategoryObject", GetT3DPath(PinType.PinSubCategoryObject.Get()));
                ValIn->SetStringField("ContainerType", GetContainerTypeString(PinType.ContainerType));
                Inputs.Add(MakeShareable(new FJsonValueObject(ValIn)));

                // Output: Exec (Then)
                TSharedPtr<FJsonObject> ExecOut = MakeShareable(new FJsonObject);
                ExecOut->SetStringField("PinName", "then");
                ExecOut->SetStringField("PinCategory", "exec");
                Outputs.Add(MakeShareable(new FJsonValueObject(ExecOut)));

                // Output: Value (Pass-through)
                TSharedPtr<FJsonObject> ValOut = MakeShareable(new FJsonObject);
                ValOut->SetStringField("PinName", "Output_" + VarName); // UE naming convention for set output
                ValOut->SetStringField("PinCategory", PinType.PinCategory.ToString());
                ValOut->SetStringField("PinSubCategory", PinType.PinSubCategory.ToString());
                ValOut->SetStringField("PinSubCategoryObject", GetT3DPath(PinType.PinSubCategoryObject.Get()));
                ValOut->SetStringField("ContainerType", GetContainerTypeString(PinType.ContainerType));
                Outputs.Add(MakeShareable(new FJsonValueObject(ValOut)));

                SetNode->SetArrayField("Inputs", Inputs);
                SetNode->SetArrayField("Outputs", Outputs);
                FullArray.Add(MakeShareable(new FJsonValueObject(SetNode)));
            }
        }
    }

    // --- MANUAL NODE INJECTION ---
    UK2Node_IfThenElse* BranchNode = NewObject<UK2Node_IfThenElse>(TempGraph);
    DumpNodeToJSON(BranchNode, "K2Node_IfThenElse", "None", "Branch");

    UK2Node_ExecutionSequence* SeqNode = NewObject<UK2Node_ExecutionSequence>(TempGraph);
    DumpNodeToJSON(SeqNode, "K2Node_ExecutionSequence", "None", "Sequence");

    // --- SAVE ---
    auto SaveJsonToFile = [](TArray<TSharedPtr<FJsonValue>>& Array, FString Path) {
        FString OutputString;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
        FJsonSerializer::Serialize(Array, Writer);
        FFileHelper::SaveStringToFile(OutputString, *Path);
        };

    FString BasePath = FPaths::GetPath(BaseFilePath);
    SaveJsonToFile(FullArray, BasePath / "UEBlueprintLibrary_Full.json");
    SaveJsonToFile(EssentialsArray, BasePath / "UEBlueprintLibrary_Essentials.json");
    SaveJsonToFile(DebugArray, BasePath / "UEBlueprintLibrary_Debug.json");

    TempBP->AddToRoot();
    TempBP->RemoveFromRoot();
}