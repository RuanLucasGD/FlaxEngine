// Copyright (c) 2012-2023 Wojciech Figat. All rights reserved.

#include "BehaviorTree.h"
#include "BehaviorTreeNode.h"
#include "BehaviorTreeNodes.h"
#include "Engine/Core/Collections/Sorting.h"
#include "Engine/Content/Factories/BinaryAssetFactory.h"
#include "Engine/Content/JsonAsset.h"
#include "Engine/Scripting/Scripting.h"
#include "Engine/Serialization/JsonSerializer.h"
#include "Engine/Serialization/MemoryReadStream.h"
#include "Engine/Threading/Threading.h"
#include "FlaxEngine.Gen.h"

REGISTER_BINARY_ASSET(BehaviorTree, "FlaxEngine.BehaviorTree", false);

bool SortBehaviorTreeChildren(GraphBox* const& a, GraphBox* const& b)
{
    // Sort by node X coordinate on surface
    auto aNode = (BehaviorTreeGraph::Node*)a->Parent;
    auto bNode = (BehaviorTreeGraph::Node*)b->Parent;
    auto aEntry = aNode->Meta.GetEntry(11);
    auto bEntry = bNode->Meta.GetEntry(11);
    auto aX = aEntry && aEntry->Data.HasItems() ? ((Float2*)aEntry->Data.Get())->X : (float)aNode->ID;
    auto bX = bEntry && bEntry->Data.HasItems() ? ((Float2*)bEntry->Data.Get())->X : (float)bNode->ID;
    return aX < bX;
}

BehaviorTreeGraphNode::~BehaviorTreeGraphNode()
{
    SAFE_DELETE(Instance);
}

bool BehaviorTreeGraph::Load(ReadStream* stream, bool loadMeta)
{
    if (VisjectGraph<BehaviorTreeGraphNode>::Load(stream, loadMeta))
        return true;

    // Build node instances hierarchy
    Node* root = nullptr;
    for (Node& node : Nodes)
    {
        if (node.Instance == Root)
        {
            root = &node;
            break;
        }
    }
    if (root)
    {
        LoadRecursive(*root);
    }

    return false;
}

void BehaviorTreeGraph::Clear()
{
    VisjectGraph<BehaviorTreeGraphNode>::Clear();

    Root = nullptr;
    NodesCount = 0;
    NodesStatesSize = 0;
}

bool BehaviorTreeGraph::onNodeLoaded(Node* n)
{
    if (n->GroupID == 19 && (n->TypeID == 1 || n->TypeID == 2 || n->TypeID == 3))
    {
        // Create node instance object
        ScriptingTypeHandle type = Scripting::FindScriptingType((StringAnsiView)n->Values[0]);
        if (!type)
            type = Scripting::FindScriptingType(StringAnsi((StringView)n->Values[0]));
        if (type)
        {
            n->Instance = (BehaviorTreeNode*)Scripting::NewObject(type);
            const Variant& data = n->Values[1];
            if (data.Type == VariantType::Blob)
                JsonSerializer::LoadFromBytes(n->Instance, Span<byte>((byte*)data.AsBlob.Data, data.AsBlob.Length), FLAXENGINE_VERSION_BUILD);

            // Find root node
            if (!Root && n->Instance && BehaviorTreeRootNode::TypeInitializer == type)
                Root = (BehaviorTreeRootNode*)n->Instance;
        }
        else
        {
            const String name = n->Values[0].ToString();
            if (name.HasChars())
                LOG(Error, "Missing type '{0}'", name);
        }
    }

    return VisjectGraph<BehaviorTreeGraphNode>::onNodeLoaded(n);
}

void BehaviorTreeGraph::LoadRecursive(Node& node)
{
    // Count total states memory size
    ASSERT_LOW_LAYER(node.Instance);
    node.Instance->_memoryOffset = NodesStatesSize;
    node.Instance->_executionIndex = NodesCount;
    NodesStatesSize += node.Instance->GetStateSize();
    NodesCount++;

    if (node.TypeID == 1 && node.Values.Count() >= 3)
    {
        // Load node decorators
        const auto& decoratorIds = node.Values[2];
        if (decoratorIds.Type.Type == VariantType::Blob && decoratorIds.AsBlob.Length)
        {
            const Span<uint32> ids((uint32*)decoratorIds.AsBlob.Data, decoratorIds.AsBlob.Length / sizeof(uint32));
            for (int32 i = 0; i < ids.Length(); i++)
            {
                Node* decorator = GetNode(ids[i]);
                if (decorator && decorator->Instance && decorator->Instance->Is<BehaviorTreeDecorator>())
                {
                    node.Instance->_decorators.Add((BehaviorTreeDecorator*)decorator->Instance);
                    decorator->Instance->_parent = node.Instance;
                    LoadRecursive(*decorator);
                }
            }
        }
    }
    if (auto* nodeCompound = ScriptingObject::Cast<BehaviorTreeCompoundNode>(node.Instance))
    {
        auto& children = node.Boxes[1].Connections;

        // Sort children from left to right (based on placement on a graph surface)
        Sorting::QuickSort(children.Get(), children.Count(), SortBehaviorTreeChildren);

        // Find all children (of output box)
        for (const GraphBox* childBox : children)
        {
            Node* child = childBox ? (Node*)childBox->Parent : nullptr;
            if (child && child->Instance)
            {
                nodeCompound->Children.Add(child->Instance);
                child->Instance->_parent = nodeCompound;
                LoadRecursive(*child);
            }
        }
    }
}

BehaviorTree::BehaviorTree(const SpawnParams& params, const AssetInfo* info)
    : BinaryAsset(params, info)
{
}

BytesContainer BehaviorTree::LoadSurface()
{
    if (WaitForLoaded())
        return BytesContainer();
    ScopeLock lock(Locker);
    if (!LoadChunks(GET_CHUNK_FLAG(0)))
    {
        const auto data = GetChunk(0);
        BytesContainer result;
        result.Copy(data->Data);
        return result;
    }
    LOG(Warning, "\'{0}\' surface data is missing.", ToString());
    return BytesContainer();
}

#if USE_EDITOR

bool BehaviorTree::SaveSurface(const BytesContainer& data)
{
    // Wait for asset to be loaded or don't if last load failed
    if (LastLoadFailed())
    {
        LOG(Warning, "Saving asset that failed to load.");
    }
    else if (WaitForLoaded())
    {
        LOG(Error, "Asset loading failed. Cannot save it.");
        return true;
    }

    ScopeLock lock(Locker);

    // Set Visject Surface data
    GetOrCreateChunk(0)->Data.Copy(data);

    // Save
    AssetInitData assetData;
    assetData.SerializedVersion = 1;
    if (SaveAsset(assetData))
    {
        LOG(Error, "Cannot save \'{0}\'", ToString());
        return true;
    }

    return false;
}

void BehaviorTree::GetReferences(Array<Guid>& output) const
{
    // Base
    BinaryAsset::GetReferences(output);

    Graph.GetReferences(output);

    // Extract refs from serialized nodes data
    for (const BehaviorTreeGraphNode& n : Graph.Nodes)
    {
        if (n.Instance == nullptr)
            continue;
        const Variant& data = n.Values[1];
        if (data.Type == VariantType::Blob)
            JsonAssetBase::GetReferences(StringAnsiView((char*)data.AsBlob.Data, data.AsBlob.Length), output);
    }
}

#endif

Asset::LoadResult BehaviorTree::load()
{
    // Load graph
    const auto surfaceChunk = GetChunk(0);
    if (surfaceChunk == nullptr)
        return LoadResult::MissingDataChunk;
    MemoryReadStream surfaceStream(surfaceChunk->Get(), surfaceChunk->Size());
    if (Graph.Load(&surfaceStream, true))
    {
        LOG(Warning, "Failed to load graph \'{0}\'", ToString());
        return LoadResult::Failed;
    }

    // Init graph
    if (Graph.Root)
    {
        Graph.Root->Init(this);
    }

    return LoadResult::Ok;
}

void BehaviorTree::unload(bool isReloading)
{
    // Clear resources
    Graph.Clear();
}

AssetChunksFlag BehaviorTree::getChunksToPreload() const
{
    return GET_CHUNK_FLAG(0);
}