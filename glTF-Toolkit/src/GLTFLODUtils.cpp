// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for license information.

#include "pch.h"

#include "GLTFTextureCompressionUtils.h"
#include "GLTFTexturePackingUtils.h"
#include "GLTFLODUtils.h"

#include "GLTFSDK/GLTF.h"
#include "GLTFSDK/GLTFConstants.h"
#include "GLTFSDK/Deserialize.h"
#include "GLTFSDK/RapidJsonUtils.h"
#include "GLTFSDK/Schema.h"

#include <algorithm>
#include <iostream>
#include <set>

using namespace Microsoft::glTF;
using namespace Microsoft::glTF::Toolkit;

const char* Microsoft::glTF::Toolkit::EXTENSION_MSFT_LOD = "MSFT_lod";
const char* Microsoft::glTF::Toolkit::MSFT_LOD_IDS_KEY = "ids";

namespace
{
    inline void AddIndexOffset(std::string& id, size_t offset)
    {
        // an empty id string indicates that the id is not inuse and therefore should not be updated
        id = (id.empty()) ? "" : std::to_string(std::stoi(id) + offset);
    }

    inline void AddIndexOffsetPacked(rapidjson::Value& json, const char* textureId, size_t offset)
    {
        if (json.HasMember(textureId))
        {
            if (json[textureId].HasMember("index"))
            {
                auto index = json[textureId]["index"].GetInt();
                json[textureId]["index"] = index + offset;
            }
        }
    }

    std::vector<std::string> ParseExtensionMSFTLod(const Node& node)
    {
        std::vector<std::string> lodIds;

        auto lodExtension = node.extensions.find(Toolkit::EXTENSION_MSFT_LOD);
        if (lodExtension != node.extensions.end())
        {
            auto json = RapidJsonUtils::CreateDocumentFromString(lodExtension->second);

            auto idIt = json.FindMember(Toolkit::MSFT_LOD_IDS_KEY);
            if (idIt != json.MemberEnd())
            {
                for (rapidjson::Value::ConstValueIterator ait = idIt->value.Begin(); ait != idIt->value.End(); ++ait)
                {
                    lodIds.push_back(std::to_string(ait->GetInt()));
                }
            }
        }

        return lodIds;
    }

    template <typename T>
    std::string SerializeExtensionMSFTLod(const T&, const std::vector<std::string>& lods, const GLTFDocument& gltfDocument)
    {
        // Omit MSFT_lod entirely if no LODs are available
        if (lods.empty())
        {
            return std::string();
        }

        rapidjson::Document doc(rapidjson::kObjectType);
        rapidjson::Document::AllocatorType& a = doc.GetAllocator();

        std::vector<size_t> lodIndices;
        lodIndices.reserve(lods.size());

        if (std::is_same<T, Material>())
        {
            for (const auto& lodId : lods)
            {
                lodIndices.push_back(ToKnownSizeType(gltfDocument.materials.GetIndex(lodId)));
            }
        }
        else if (std::is_same<T, Node>())
        {
            for (const auto& lodId : lods)
            {
                lodIndices.push_back(ToKnownSizeType(gltfDocument.nodes.GetIndex(lodId)));
            }
        }
        else
        {
            throw GLTFException("LODs can only be applied to materials or nodes.");
        }

        doc.AddMember(RapidJsonUtils::ToStringValue(Toolkit::MSFT_LOD_IDS_KEY, a), RapidJsonUtils::ToJsonArray(lodIndices, a), a);

        rapidjson::StringBuffer stringBuffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(stringBuffer);
        doc.Accept(writer);

        return stringBuffer.GetString();
    }

    GLTFDocument AddGLTFNodeLOD(const GLTFDocument& primary, LODMap& primaryLods, const GLTFDocument& lod)
    {
        Microsoft::glTF::GLTFDocument gltfLod(primary);

        auto primaryScenes = primary.scenes.Elements();
        auto lodScenes = lod.scenes.Elements();

        size_t MaxLODLevel = 0;

        // Both GLTF must have equivalent number and order of scenes and root nodes per scene otherwise merge will not be possible
        bool sceneNodeMatch = false;
        if (primaryScenes.size() == lodScenes.size())
        {
            for (size_t sceneIdx = 0; sceneIdx < primaryScenes.size(); sceneIdx++)
            {

                if ((primaryScenes[sceneIdx].nodes.size() == lodScenes[sceneIdx].nodes.size()) &&
                    (lodScenes[sceneIdx].nodes.size() == 1 ||
                        std::equal(primaryScenes[sceneIdx].nodes.begin(), primaryScenes[sceneIdx].nodes.end(), lodScenes[sceneIdx].nodes.begin()))
                    )
                {
                    sceneNodeMatch = true;
                    auto primaryRootNode = gltfLod.nodes.Get(primaryScenes[sceneIdx].nodes[0]);
                    MaxLODLevel = std::max(MaxLODLevel, primaryLods.at(primaryRootNode.id)->size());
                }
                else
                {
                    sceneNodeMatch = false;
                    break;
                }
            }
        }

        MaxLODLevel++;

        if (!sceneNodeMatch || primaryScenes.empty())
        {
            // Mis-match or empty scene; either way cannot merge Lod in
            throw new std::runtime_error("Primary Scene either empty or does not match scene node count of LOD gltf");
        }

        std::string nodeLodLabel = "_lod" + std::to_string(MaxLODLevel);

        // lod merge is performed from the lowest reference back upwards
        // e.g. buffers/samplers/extensions do not reference any other part of the gltf manifest    
        size_t buffersOffset = gltfLod.buffers.Size();
        size_t samplersOffset = gltfLod.samplers.Size();
        {
            auto lodBuffers = lod.buffers.Elements();
            for (auto buffer : lodBuffers)
            {
                AddIndexOffset(buffer.id, buffersOffset);
                gltfLod.buffers.Append(std::move(buffer));
            }

            auto lodSamplers = lod.samplers.Elements();
            for (auto sampler : lodSamplers)
            {
                AddIndexOffset(sampler.id, samplersOffset);
                gltfLod.samplers.Append(std::move(sampler));
            }

            for (const auto& extension : lod.extensionsUsed)
            {
                gltfLod.extensionsUsed.insert(extension);
            }
            // ensure that MSFT_LOD extension is specified as being used
            gltfLod.extensionsUsed.insert(Toolkit::EXTENSION_MSFT_LOD);
        }

        size_t accessorOffset = gltfLod.accessors.Size();
        size_t texturesOffset = gltfLod.textures.Size();
        {
            // Buffer Views depend upon Buffers
            size_t bufferViewsOffset = gltfLod.bufferViews.Size();
            auto lodBufferViews = lod.bufferViews.Elements();
            for (auto bufferView : lodBufferViews)
            {
                AddIndexOffset(bufferView.id, bufferViewsOffset);
                AddIndexOffset(bufferView.bufferId, buffersOffset);
                gltfLod.bufferViews.Append(std::move(bufferView));
            }

            // Accessors depend upon Buffer views        
            auto lodAccessors = lod.accessors.Elements();
            for (auto accessor : lodAccessors)
            {
                AddIndexOffset(accessor.id, accessorOffset);
                AddIndexOffset(accessor.bufferViewId, bufferViewsOffset);
                gltfLod.accessors.Append(std::move(accessor));
            }

            // Images depend upon Buffer views
            size_t imageOffset = gltfLod.images.Size();
            auto lodImages = lod.images.Elements();
            for (auto image : lodImages)
            {
                AddIndexOffset(image.id, imageOffset);
                AddIndexOffset(image.bufferViewId, bufferViewsOffset);
                gltfLod.images.Append(std::move(image));
            }

            // Textures depend upon Samplers and Images
            auto lodTextures = lod.textures.Elements();
            for (auto texture : lodTextures)
            {
                AddIndexOffset(texture.id, texturesOffset);
                AddIndexOffset(texture.samplerId, samplersOffset);
                AddIndexOffset(texture.imageId, imageOffset);

                // MSFT_texture_dds extension
                auto ddsExtensionIt = texture.extensions.find(EXTENSION_MSFT_TEXTURE_DDS);
                if (ddsExtensionIt != texture.extensions.end() && !ddsExtensionIt->second.empty())
                {
                    rapidjson::Document ddsJson = RapidJsonUtils::CreateDocumentFromString(ddsExtensionIt->second);

                    if (ddsJson.HasMember("source"))
                    {
                        auto index = ddsJson["source"].GetInt();
                        ddsJson["source"] = index + imageOffset;
                    }

                    rapidjson::StringBuffer buffer;
                    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                    ddsJson.Accept(writer);

                    ddsExtensionIt->second = buffer.GetString();
                }

                gltfLod.textures.Append(std::move(texture));
            }
        }

        // Material Merge
        // Note the extension KHR_materials_pbrSpecularGlossiness will be also updated
        // Materials depend upon textures
        size_t materialOffset = gltfLod.materials.Size();
        {
            auto lodMaterials = lod.materials.Elements();
            for (auto material : lodMaterials)
            {
                // post-fix with lod level indication; 
                // no functional reason other than making it easier to natively read gltf files with lods
                material.name += nodeLodLabel;
                AddIndexOffset(material.id, materialOffset);

                AddIndexOffset(material.normalTexture.id, texturesOffset);
                AddIndexOffset(material.occlusionTexture.id, texturesOffset);
                AddIndexOffset(material.emissiveTextureId, texturesOffset);

                AddIndexOffset(material.metallicRoughness.baseColorTextureId, texturesOffset);
                AddIndexOffset(material.metallicRoughness.metallicRoughnessTextureId, texturesOffset);

                AddIndexOffset(material.specularGlossiness.diffuseTextureId, texturesOffset);
                AddIndexOffset(material.specularGlossiness.specularGlossinessTextureId, texturesOffset);

                // MSFT_packing_occlusionRoughnessMetallic packed textures
                auto ormExtensionIt = material.extensions.find(EXTENSION_MSFT_PACKING_ORM);
                if (ormExtensionIt != material.extensions.end() && !ormExtensionIt->second.empty())
                {
                    rapidjson::Document ormJson = RapidJsonUtils::CreateDocumentFromString(ormExtensionIt->second);

                    AddIndexOffsetPacked(ormJson, "occlusionRoughnessMetallicTexture", texturesOffset);
                    AddIndexOffsetPacked(ormJson, "roughnessMetallicOcclusionTexture", texturesOffset);
                    AddIndexOffsetPacked(ormJson, "normalTexture", texturesOffset);

                    rapidjson::StringBuffer buffer;
                    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                    ormJson.Accept(writer);

                    ormExtensionIt->second = buffer.GetString();
                }

                gltfLod.materials.Append(std::move(material));
            }
        }

        // Meshs depend upon Accessors and Materials
        size_t meshOffset = gltfLod.meshes.Size();
        {
            auto lodMeshes = lod.meshes.Elements();
            for (auto mesh : lodMeshes)
            {
                // post-fix with lod level indication; 
                // no functional reason other than making it easier to natively read gltf files with lods
                mesh.name += nodeLodLabel;
                AddIndexOffset(mesh.id, meshOffset);

                for (auto Itr = mesh.primitives.begin(); Itr != mesh.primitives.end(); Itr++)
                {
                    AddIndexOffset(Itr->positionsAccessorId, accessorOffset);
                    AddIndexOffset(Itr->normalsAccessorId, accessorOffset);
                    AddIndexOffset(Itr->indicesAccessorId, accessorOffset);
                    AddIndexOffset(Itr->uv0AccessorId, accessorOffset);
                    AddIndexOffset(Itr->uv1AccessorId, accessorOffset);
                    AddIndexOffset(Itr->color0AccessorId, accessorOffset);

                    AddIndexOffset(Itr->materialId, materialOffset);
                }

                gltfLod.meshes.Append(std::move(mesh));
            }
        }

        // Nodes depend upon Nodes and Meshes
        size_t nodeOffset = gltfLod.nodes.Size();
        {
            auto nodes = lod.nodes.Elements();
            for (auto node : nodes)
            {
                // post-fix with lod level indication; 
                // no functional reason other than making it easier to natively read gltf files with lods
                node.name += nodeLodLabel;
                AddIndexOffset(node.id, nodeOffset);
                AddIndexOffset(node.meshId, meshOffset);

                for (auto Itr = node.children.begin(); Itr != node.children.end(); Itr++)
                {
                    AddIndexOffset(*Itr, nodeOffset);
                }

                gltfLod.nodes.Append(std::move(node));
            };
        }

        // update the primary GLTF root nodes lod extension to reference the new lod root node
        // N.B. new lods are always added to the back
        for (size_t sceneIdx = 0; sceneIdx < primaryScenes.size(); sceneIdx++)
        {
            for (size_t rootNodeIdx = 0; rootNodeIdx < primaryScenes[sceneIdx].nodes.size(); rootNodeIdx++)
            {
                auto idx = primaryScenes[sceneIdx].nodes[rootNodeIdx];
                Node nodeWithLods(gltfLod.nodes.Get(idx));
                int lodRootIdx = std::stoi(lodScenes[sceneIdx].nodes[rootNodeIdx]) + static_cast<int>(nodeOffset);
                auto primaryNodeLod = primaryLods.at(nodeWithLods.id);
                primaryNodeLod->emplace_back(std::to_string(lodRootIdx));
            }
        }

        return gltfLod;
    }
}

LODMap GLTFLODUtils::ParseDocumentNodeLODs(const GLTFDocument& doc)
{
    LODMap lodMap;

    for (auto node : doc.nodes.Elements())
    {
        lodMap.emplace(node.id, std::move(std::make_shared<std::vector<std::string>>(ParseExtensionMSFTLod(node))));
    }

    return lodMap;
}

GLTFDocument GLTFLODUtils::MergeDocumentsAsLODs(const std::vector<GLTFDocument>& docs)
{
    if (docs.empty())
    {
        throw std::invalid_argument("MergeDocumentsAsLODs passed empty vector");
    }

    GLTFDocument gltfPrimary(docs[0]);
    LODMap lods = ParseDocumentNodeLODs(gltfPrimary);

    for (size_t i = 1; i < docs.size(); i++)
    {
        gltfPrimary = AddGLTFNodeLOD(gltfPrimary, lods, docs[i]);
    }

    for (auto lod : lods)
    {
        if (lod.second == nullptr || lod.second->size() == 0)
        {
            continue;
        }

        auto node = gltfPrimary.nodes.Get(lod.first);

        auto lodExtensionValue = SerializeExtensionMSFTLod<Node>(node, *lod.second, gltfPrimary);
        if (!lodExtensionValue.empty())
        {
            node.extensions.emplace(EXTENSION_MSFT_LOD, lodExtensionValue);
            gltfPrimary.nodes.Replace(node);
        }
    }

    return gltfPrimary;
}

GLTFDocument GLTFLODUtils::MergeDocumentsAsLODs(const std::vector<GLTFDocument>& docs, const std::vector<double>& screenCoveragePercentages)
{
    GLTFDocument merged = MergeDocumentsAsLODs(docs);

    if (screenCoveragePercentages.size() == 0)
    {
        return merged;
    }

    for (auto scene : merged.scenes.Elements())
    {
        for (auto rootNodeIndex : scene.nodes)
        {
            auto primaryRootNode = merged.nodes.Get(rootNodeIndex);

            rapidjson::Document extrasJson(rapidjson::kObjectType);
            if (!primaryRootNode.extras.empty())
            {
                extrasJson.Parse(primaryRootNode.extras.c_str());
            }
            rapidjson::Document::AllocatorType& allocator = extrasJson.GetAllocator();

            rapidjson::Value screenCoverageArray = RapidJsonUtils::ToJsonArray(screenCoveragePercentages, allocator);

            extrasJson.AddMember("MSFT_screencoverage", screenCoverageArray, allocator);

            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            extrasJson.Accept(writer);

            primaryRootNode.extras = buffer.GetString();

            merged.nodes.Replace(primaryRootNode);
        }
    }

    return merged;
}

uint32_t GLTFLODUtils::NumberOfNodeLODLevels(const GLTFDocument& doc, const LODMap& lods)
{
    size_t maxLODLevel = 0;
    for (auto node : doc.nodes.Elements())
    {
        maxLODLevel = std::max(maxLODLevel, lods.at(node.id)->size());
    }

    return static_cast<uint32_t>(maxLODLevel);
}

