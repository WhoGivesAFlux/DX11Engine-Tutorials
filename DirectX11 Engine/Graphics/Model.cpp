#include "Model.h"

bool Model::Init(const std::string& filepath, ID3D11Device* device, ID3D11DeviceContext* deviceContext, ConstantBuffer<CB_VS_vertexShader>& cb_vs_vertexShader)
{
	this->device = device;
	this->deviceContext = deviceContext;
	this->cb_vs_vertexShader = &cb_vs_vertexShader;
	try
	{
		if (!this->LoadModel(filepath))
			return false;
	}
	catch (COMException & exception)
	{
		ErrorLogger::Log(exception);
		return false;
	}
	return true;
}

void Model::Draw(const XMMATRIX& worldMatrix, const XMMATRIX& viewProjectionMatrix)
{
	this->deviceContext->VSSetConstantBuffers(0, 1, this->cb_vs_vertexShader->GetAddressOf());

	for (size_t i = 0; i < meshes.size(); ++i)
	{
		this->cb_vs_vertexShader->data.wvpMatrix = meshes[i].GetTransformMatrix() * worldMatrix * viewProjectionMatrix;
		this->cb_vs_vertexShader->data.worldMatrix = meshes[i].GetTransformMatrix() * worldMatrix;
		this->cb_vs_vertexShader->ApplyChanges();
		meshes[i].Draw();
	}

}

bool Model::LoadModel(const std::string& filepath)
{
	this->directory = StringToolkit::GetDirectoryFromPath(filepath);

	Assimp::Importer importer;

	const aiScene* pScene = importer.ReadFile(filepath, aiProcess_Triangulate | aiProcess_ConvertToLeftHanded | aiComponent_TANGENTS_AND_BITANGENTS);

	if (pScene == NULL)
		return false;

	this->ProcessNode(pScene->mRootNode, pScene, DirectX::XMMatrixIdentity());
	return true;
}

void Model::ProcessNode(aiNode* node, const aiScene* scene, const XMMATRIX& parentTransformMatrix)
{
	XMMATRIX nodeTransformMatrix = XMMatrixTranspose(static_cast<XMMATRIX>(&node->mTransformation.a1))* parentTransformMatrix;

	for (UINT i = 0; i < node->mNumMeshes; ++i)
	{
		aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
		meshes.push_back(this->ProcessMesh(mesh, scene, nodeTransformMatrix));
	}

	for (UINT i = 0; i < node->mNumChildren; ++i)
	{
		this->ProcessNode(node->mChildren[i], scene, nodeTransformMatrix);
	}
}

Mesh Model::ProcessMesh(aiMesh* mesh, const aiScene* scene, const XMMATRIX& transformMatrix)
{
	std::vector<Vertex> vertices;
	std::vector<DWORD> indices;

	for (UINT i = 0; i < mesh->mNumVertices; ++i)
	{
		Vertex vertex;

		vertex.pos.x = mesh->mVertices[i].x;
		vertex.pos.y = mesh->mVertices[i].y;
		vertex.pos.z = mesh->mVertices[i].z;

		vertex.normal.x = mesh->mNormals[i].x;
		vertex.normal.y = mesh->mNormals[i].y;
		vertex.normal.z = mesh->mNormals[i].z;

		if (mesh->mTextureCoords[0]) //index 0 is main texture
		{
			vertex.texCoord.x = static_cast<float>(mesh->mTextureCoords[0][i].x);
			vertex.texCoord.y = static_cast<float>(mesh->mTextureCoords[0][i].y);
		}

		vertices.push_back(vertex);
	}

	for (UINT i = 0; i < mesh->mNumFaces; ++i)
	{
		aiFace face = mesh->mFaces[i];
		for (UINT j = 0; j < face.mNumIndices; ++j)
		{
			indices.push_back(face.mIndices[j]);
		}
	}

	std::vector<Texture> textures;
	aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
	std::vector<Texture> pointTextures = LoadMaterialTextures(material, aiTextureType::aiTextureType_DIFFUSE, scene);
	textures.insert(textures.end(), pointTextures.begin(), pointTextures.end());

	return Mesh(this->device, this->deviceContext, vertices, indices, textures, transformMatrix);
}

std::vector<Texture> Model::LoadMaterialTextures(aiMaterial* pMaterial, aiTextureType textureType, const aiScene* pScene)
{
	std::vector<Texture> materialTextures;
	TextureStorageType storeType = TextureStorageType::INVALID;
	unsigned int textureCount = pMaterial->GetTextureCount(textureType);

	if (textureCount == 0)
	{
		storeType = TextureStorageType::NONE;
		aiColor3D aiColor(0.0f, 0.0f, 0.0f);
		switch (textureType)
		{
		case aiTextureType_DIFFUSE:
			pMaterial->Get(AI_MATKEY_COLOR_DIFFUSE, aiColor);
			if (aiColor.IsBlack())
			{
				materialTextures.push_back(Texture(this->device, Colors::UnloadedTextureColor, textureType));
				return materialTextures;
			}
			materialTextures.push_back(Texture(this->device, Color(aiColor.r * 255, aiColor.g * 255, aiColor.b * 255), textureType));
			return materialTextures;
		}
	}
	else
	{
		for (UINT i = 0; i < textureCount; ++i)
		{
			aiString path;
			pMaterial->GetTexture(textureType, i, &path);
			TextureStorageType storeType = DetermineTextureStorageType(pScene, pMaterial, i, textureType);
			switch (storeType)
			{
			case TextureStorageType::DISK:
			{
				std::string filename = this->directory + '\\' + path.C_Str();
				Texture diskTexture(this->device, filename, textureType);
				materialTextures.push_back(diskTexture);
				break;
			}
			case TextureStorageType::EMBEDDED_COMPRESSED:
			{
				const aiTexture* pTexture = pScene->GetEmbeddedTexture(path.C_Str());
				Texture embeddedTexture(this->device, reinterpret_cast<uint8_t*>(pTexture->pcData), pTexture->mWidth, textureType);
				materialTextures.push_back(embeddedTexture);
				break;
			}
			case TextureStorageType::EMBEDDED_INDEX_COMPRESSED:
			{
				int index = GetTextureIndex(&path);
				Texture embeddedIndexedTexture(this->device, reinterpret_cast<uint8_t*>(pScene->mTextures[index]->pcData), pScene->mTextures[index]->mWidth, textureType);
				materialTextures.push_back(embeddedIndexedTexture);
				break;
			}
			}
		}
	}
	if(materialTextures.size() == 0)
		materialTextures.push_back(Texture(this->device, Colors::UnhandledTextureColor, aiTextureType::aiTextureType_DIFFUSE));
	return materialTextures;
}

TextureStorageType Model::DetermineTextureStorageType(const aiScene* pScene, aiMaterial* pMaterial, unsigned int index, aiTextureType textureType)
{
	if(pMaterial->GetTextureCount(textureType) == 0)
		return TextureStorageType::NONE;

	aiString path;
	pMaterial->GetTexture(textureType, index, &path);
	std::string texturePath = path.C_Str();

	if (texturePath[0] == '*')
	{
		if (pScene->mTextures[0]->mHeight == 0)
			return TextureStorageType::EMBEDDED_INDEX_COMPRESSED;
		else
		{
			assert("SUPPORT DOES NOT YET EXIST FOR EMBEDDED INDEXED NON COMPRESSED TEXTURES!" && 0);
			return TextureStorageType::EMBEDDED_INDEX_NON_COMPRESSED;
		}
	}
	if (auto pTex = pScene->GetEmbeddedTexture(texturePath.c_str()))
	{
		if (pTex->mHeight == 0)
			return TextureStorageType::EMBEDDED_COMPRESSED;
		else
		{
			assert("SUPPORT DOES NOT YET EXIST FOR EMBEDDED NON COMPRESSED TEXTURES!" && 0);
			return TextureStorageType::EMBEDDED_NON_COMPRESSED;
		}
	}
	if (texturePath.find('.') != std::string::npos)
	{
		return TextureStorageType::DISK;
	}

	return TextureStorageType::NONE;
}

int Model::GetTextureIndex(aiString* pStr)
{
	assert(pStr->length >= 2);
	return atoi(&pStr->C_Str()[1]);
}
