#ifdef FOSTER_D3D11_ENABLED

#include "foster_renderer.h"
#include "foster_internal.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <SDL_syswm.h>

// Configuration options for this renderer
#define FOSTER_VERTEX_SHADER_MODEL "vs_5_0"
#define FOSTER_PIXEL_SHADER_MODEL "ps_5_0"
#define FOSTER_GROWTH_BLOCK_SIZE 16

// helpers
#define CHECK_RESULT_A(hr, message, returnValue, ...) \
    if (!SUCCEEDED(hr)) \
    { \
        FosterLogError(message ": result %#08X", hr, __VA_ARGS__); \
        return returnValue; \
    }

#define CHECK_RESULT_V(hr, message, ...) \
    if (!SUCCEEDED(hr)) FosterLogError(message ": result %#08X", hr, __VA_ARGS__);

#define CHECK_A(condition, message, returnValue, ...) \
    if (!condition) \
    { \
        FosterLogError(message, __VA_ARGS__); \
        return returnValue; \
    }

#define CHECK_V(condition, message, ...) \
    if (!condition) FosterLogError(message, __VA_ARGS__);

#define CHECK(condition, message) CHECK_A(condition, message, false)
#define CHECK_RESULT(hr, message) CHECK_RESULT_A(hr, message, false)

#define FOSTER_RECT_EQUAL(a, b) ((a).x == (b).x && (a).y == (b).y && (a).w == (b).w && (a).h == (b).h)

typedef struct FosterTexture_D3D11
{
    ID3D11Texture2D* texture;
    ID3D11ShaderResourceView* view;
    FosterTextureSampler sampler;
    int width;
    int height;
    DXGI_FORMAT dxgiFormat;

    // Because Shader uniforms assign textures, it's possible for the user to
    // dispose of a texture but still have it assigned in a shader. Thus we use
    // a simple ref counter to determine when it's safe to delete the wrapping
    // texture class.
    int refCount;
    int disposed;
} FosterTexture_D3D11;

typedef struct FosterTarget_D3D11
{
    ID3D11DepthStencilView* depthView;
    int width;
    int height;
    int colorViewCount;
    ID3D11RenderTargetView* colorViews[FOSTER_MAX_TARGET_ATTACHMENTS];
    FosterTexture_D3D11* colorTextures[FOSTER_MAX_TARGET_ATTACHMENTS];
    FosterTexture_D3D11* depthTexture;
} FosterTarget_D3D11;

typedef struct FosterUniform_D3D11
{
    char* name;
    unsigned int index;
    unsigned int size;
    unsigned int offset;
    int type;
} FosterUniform_D3D11;

typedef struct FosterShaderInput_D3D11
{
    char* name;
    unsigned int index;
} FosterShaderInput_D3D11;

typedef struct FosterSampler_D3D11
{
    ID3D11SamplerState* sampler;
    FosterTextureSampler* settings;
} FosterSampler_D3D11;

typedef struct FosterShader_D3D11
{
    ID3D11VertexShader* vertexShader;
    ID3D11PixelShader* pixelShader;
    ID3D11Buffer* uniformBuffer;
    int uniformCount;
    FosterUniform_D3D11* uniforms;
    FosterTexture_D3D11* textures[FOSTER_MAX_UNIFORM_TEXTURES];
    FosterSampler_D3D11* samplers[FOSTER_MAX_UNIFORM_TEXTURES];
    unsigned int inputCount;
    FosterShaderInput_D3D11* inputs;
    ID3DBlob* vertexBlob;
} FosterShader_D3D11;

typedef struct FosterMesh_D3D11
{
    ID3D11Buffer* vertexBuffer;
    ID3D11Buffer* indexBuffer;
    int vertexBytes;
    int indexBytes;
    int vertexSize;
    int indexSize;
    FosterVertexFormat vertexFormat;
    FosterIndexFormat indexFormat;
} FosterMesh_D3D11;

/*
 * Since D3D11 demands an InputLayout, and InputLayouts are validated to each shader,
 * they cannot be feasibly stored to the mesh they are assigned with. Instead, we cache them
 * and set them up in a way to be grouped by shader - Multiple meshes can share the same
 * layout, so we can reuse them. They are managed via reference counting.
 * If a shader is destroyed, all InputLayouts associated with it shall also be destroyed.
 */
typedef struct FosterLayoutInstance_D3D11
{
    FosterShader_D3D11* shader;
    ID3D11InputLayout* layout;
} FosterLayoutInstance_D3D11;

typedef struct FosterLayoutGroup_D3D11
{
    FosterVertexFormat format;
    FosterLayoutInstance_D3D11* instances;
    int count;
    int references;
} FosterLayoutGroup_D3D11;

typedef struct FosterLayoutCache_D3D11
{
    FosterLayoutGroup_D3D11* layouts;
    int count;
} FosterLayoutCache_D3D11;

typedef struct FosterDX11State
{
    bool stateInitializing;
	ID3D11Device* device;
	ID3D11DeviceContext* context;
	IDXGISwapChain* swapChain;
	ID3D11RenderTargetView* backBufferView;
	ID3D11DepthStencilView* backBufferDepthView;
    ID3D11DepthStencilState* depthState;
	D3D_FEATURE_LEVEL featureLevel;
    ID3D11BlendState* blendState;
    ID3D11RasterizerState* rasterizerState;
    D3D11_BLEND_DESC blendDesc;

    FosterLayoutCache_D3D11 layoutCache;

    ID3D11Buffer* stateVertexBuffer;
    ID3D11Buffer* stateIndexBuffer;
    FosterTexture_D3D11* stateTextureSlots[FOSTER_MAX_UNIFORM_TEXTURES];
    FosterSampler_D3D11* stateSamplerSlots[FOSTER_MAX_UNIFORM_TEXTURES];
    ID3D11RenderTargetView* stateFrameBuffer;
    FosterShader_D3D11* stateShader;
    int stateFrameBufferWidth;
    int stateFrameBufferHeight;
    FosterRect stateViewport;
    FosterRect stateScissor;
    FosterBlend stateBlend;
    FosterCompare stateCompare;
    bool stateBlendEnabled;
    FosterCull stateCull;
    FosterVertexFormat stateVertexFormat;

    int max_renderbuffer_size;
    int max_texture_image_units;
    int max_texture_size;
} FosterDX11State;
static FosterDX11State fdx;

// conversion methods
D3D11_TEXTURE_ADDRESS_MODE FosterWrapToD3D11(FosterTextureWrap wrap)
{
    switch (wrap)
    {
    case FOSTER_TEXTURE_WRAP_REPEAT:
        return D3D11_TEXTURE_ADDRESS_WRAP;
    case FOSTER_TEXTURE_WRAP_CLAMP_TO_EDGE:
        return D3D11_TEXTURE_ADDRESS_CLAMP;
    case FOSTER_TEXTURE_WRAP_MIRRORED_REPEAT:
        return D3D11_TEXTURE_ADDRESS_MIRROR;
    default:
        return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

D3D11_FILTER FosterFilterToD3D11(FosterTextureFilter filter)
{
    switch (filter)
    {
    case FOSTER_TEXTURE_FILTER_NEAREST:
        return D3D11_FILTER_MIN_MAG_MIP_POINT;
    case FOSTER_TEXTURE_FILTER_LINEAR:
        return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    default:
        return D3D11_FILTER_MIN_MAG_MIP_POINT;
    }
}

D3D11_BLEND_OP FosterBlendOpToD3D11(FosterBlendOp operation)
{
    switch (operation)
    {
    case FOSTER_BLEND_OP_ADD:
        return D3D11_BLEND_OP_ADD;
    case FOSTER_BLEND_OP_SUBTRACT:
        return D3D11_BLEND_OP_SUBTRACT;
    case FOSTER_BLEND_OP_REVERSE_SUBTRACT:
        return D3D11_BLEND_OP_REV_SUBTRACT;
    case FOSTER_BLEND_OP_MIN:
        return D3D11_BLEND_OP_MIN;
    case FOSTER_BLEND_OP_MAX:
        return D3D11_BLEND_OP_MAX;
    default:
        return D3D11_BLEND_OP_ADD;
    }
}

D3D11_BLEND FosterBlendFactorToD3D11(FosterBlendFactor factor)
{
    switch (factor)
    {
    case FOSTER_BLEND_FACTOR_Zero:
        return D3D11_BLEND_ZERO;
    case FOSTER_BLEND_FACTOR_One:
        return D3D11_BLEND_ONE;
    case FOSTER_BLEND_FACTOR_SrcColor:
        return D3D11_BLEND_SRC_COLOR;
    case FOSTER_BLEND_FACTOR_OneMinusSrcColor:
        return D3D11_BLEND_INV_SRC_COLOR;
    case FOSTER_BLEND_FACTOR_DstColor:
        return D3D11_BLEND_DEST_COLOR;
    case FOSTER_BLEND_FACTOR_OneMinusDstColor:
        return D3D11_BLEND_INV_DEST_COLOR;
    case FOSTER_BLEND_FACTOR_SrcAlpha:
        return D3D11_BLEND_SRC_ALPHA;
    case FOSTER_BLEND_FACTOR_OneMinusSrcAlpha:
        return D3D11_BLEND_INV_SRC_ALPHA;
    case FOSTER_BLEND_FACTOR_DstAlpha:
        return D3D11_BLEND_DEST_ALPHA;
    case FOSTER_BLEND_FACTOR_OneMinusDstAlpha:
        return D3D11_BLEND_INV_DEST_ALPHA;
    case FOSTER_BLEND_FACTOR_ConstantColor:
        return D3D11_BLEND_BLEND_FACTOR;
    case FOSTER_BLEND_FACTOR_OneMinusConstantColor:
        return D3D11_BLEND_INV_BLEND_FACTOR;
    case FOSTER_BLEND_FACTOR_ConstantAlpha:
        return D3D11_BLEND_BLEND_FACTOR;
    case FOSTER_BLEND_FACTOR_OneMinusConstantAlpha:
        return D3D11_BLEND_INV_BLEND_FACTOR;
    case FOSTER_BLEND_FACTOR_SrcAlphaSaturate:
        return D3D11_BLEND_SRC_ALPHA_SAT;
    default:
        return D3D11_BLEND_ZERO;
    }
}

FosterUniformType FosterUniformTypeFromD3D11(D3D11_SHADER_TYPE_DESC* desc)
{
    switch (desc->Type)
    {
    case D3D_SVT_FLOAT:
        if (desc->Rows == 1)
        {
            if (desc->Columns == 1) return FOSTER_UNIFORM_TYPE_FLOAT;
            if (desc->Columns == 2) return FOSTER_UNIFORM_TYPE_FLOAT2;
            if (desc->Columns == 3) return FOSTER_UNIFORM_TYPE_FLOAT3;
            if (desc->Columns == 4) return FOSTER_UNIFORM_TYPE_FLOAT4;
        }
        else if (desc->Rows == 2 && desc->Columns == 3)
        {
            return FOSTER_UNIFORM_TYPE_MAT3X2;
        }
        else if (desc->Rows == 4 && desc->Columns == 4)
        {
            return FOSTER_UNIFORM_TYPE_MAT4X4;
        }
    case D3D_SVT_SAMPLER:
        return FOSTER_UNIFORM_TYPE_SAMPLER2D;
    case D3D_SVT_TEXTURE2D:
        return FOSTER_UNIFORM_TYPE_TEXTURE2D;
    default:
        return FOSTER_UNIFORM_TYPE_NONE;
    }
}

DXGI_FORMAT FosterVertexTypeToD3D11(FosterVertexType type)
{
    switch (type)
    {
    case FOSTER_VERTEX_TYPE_NONE:
        return DXGI_FORMAT_UNKNOWN;
    case FOSTER_VERTEX_TYPE_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case FOSTER_VERTEX_TYPE_FLOAT2:
        return DXGI_FORMAT_R32G32_FLOAT;
    case FOSTER_VERTEX_TYPE_FLOAT3:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case FOSTER_VERTEX_TYPE_FLOAT4:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case FOSTER_VERTEX_TYPE_BYTE4:
        return DXGI_FORMAT_R8G8B8A8_SINT;
    case FOSTER_VERTEX_TYPE_UBYTE4:
        return DXGI_FORMAT_R8G8B8A8_UINT;
    case FOSTER_VERTEX_TYPE_SHORT2:
        return DXGI_FORMAT_R16G16_SINT;
    case FOSTER_VERTEX_TYPE_USHORT2:
        return DXGI_FORMAT_R16G16_UINT;
    case FOSTER_VERTEX_TYPE_SHORT4:
        return DXGI_FORMAT_R16G16B16A16_SINT;
    case FOSTER_VERTEX_TYPE_USHORT4:
        return DXGI_FORMAT_R16G16B16A16_UINT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}


D3D11_CULL_MODE FosterCullModeToD3D11(FosterCull mode)
{
    switch (mode)
    {
    case FOSTER_CULL_NONE:
        return D3D11_CULL_NONE;
    case FOSTER_CULL_FRONT:
        return D3D11_CULL_FRONT;
    case FOSTER_CULL_BACK:
        return D3D11_CULL_BACK;
    default:
        return D3D11_CULL_NONE;
    }
}

void FosterBindShader_D3D11(FosterShader_D3D11* shader)
{
    if (fdx.stateInitializing || fdx.stateShader != shader)
    {
        fdx.stateShader = shader;
        ID3D11DeviceContext_VSSetShader(fdx.context, shader->vertexShader, NULL, 0);
        ID3D11DeviceContext_PSSetShader(fdx.context, shader->pixelShader, NULL, 0);
        if (shader->uniformBuffer)
            ID3D11DeviceContext_VSSetConstantBuffers(fdx.context, 0, 1, &shader->uniformBuffer);
    }

    // bind textures, samplers
    for (int i = 0; i < FOSTER_MAX_UNIFORM_TEXTURES; i++)
    {
        if (shader->textures[i] && fdx.stateTextureSlots[i] != shader->textures[i])
        {
            fdx.stateTextureSlots[i] = shader->textures[i];
            ID3D11DeviceContext_PSSetShaderResources(fdx.context, i, 1, (ID3D11ShaderResourceView**)&shader->textures[i]->view);
        }
        if (shader->samplers[i] && fdx.stateSamplerSlots[i] != shader->samplers[i])
        {
            fdx.stateSamplerSlots[i] = shader->samplers[i];
            ID3D11DeviceContext_PSSetSamplers(fdx.context, i, 1, &shader->samplers[i]->sampler);
        }
    }
}

bool FosterVertexFormatEqual(FosterVertexFormat a, FosterVertexFormat b)
{
    if (a.stride != b.stride || a.elementCount != b.elementCount)
        return false;
    for (int i = 0; i < a.elementCount; i++)
    {
        if (a.elements[i].type != b.elements[i].type ||
            a.elements[i].index != b.elements[i].index ||
            a.elements[i].normalized != b.elements[i].normalized)
            return false;
    }
    return true;
}

FosterLayoutGroup_D3D11* FosterGetLayoutGroup(FosterVertexFormat format, int* index)
{
    for (int i = 0; i < fdx.layoutCache.count; i++)
    {
        if (FosterVertexFormatEqual(fdx.layoutCache.layouts[i].format, format))
        {
            if (index)
                *index = i;
            return &fdx.layoutCache.layouts[i];
        }
    }
    return NULL;
}

ID3D11InputLayout* FosterGetLayoutInstance(FosterShader_D3D11* shader, FosterVertexFormat format)
{
    FosterLayoutGroup_D3D11* group = FosterGetLayoutGroup(format, NULL);
    if (group == NULL)
        return NULL;

    for (int i = 0; i < group->count; i++)
    {
        if (group->instances[i].shader == shader)
        {
            if(group->instances[i].layout)
                return group->instances[i].layout;
        }
    }

    // create
    D3D11_INPUT_ELEMENT_DESC* elements = SDL_malloc(sizeof(D3D11_INPUT_ELEMENT_DESC) * format.elementCount);
    int byteOffset = 0;
    for (int i = 0; i < format.elementCount; i++)
    {
        DXGI_FORMAT dxgiFormat = FosterVertexTypeToD3D11(format.elements[i].type);
        // fetch semantic name from shader inputs (from struct)
        elements[i].SemanticName = (const char*)shader->inputs[i].name;
        elements[i].SemanticIndex = shader->inputs[i].index;
        elements[i].Format = dxgiFormat;
        elements[i].InputSlot = 0;
        elements[i].AlignedByteOffset = byteOffset;
        elements[i].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        elements[i].InstanceDataStepRate = 0;

        // find and add size
        switch (format.elements[i].type)
        {
            case FOSTER_VERTEX_TYPE_FLOAT:
                byteOffset += 4;
                break;
            case FOSTER_VERTEX_TYPE_FLOAT2:
                byteOffset += 8;
                break;
            case FOSTER_VERTEX_TYPE_FLOAT3:
                byteOffset += 12;
                break;
            case FOSTER_VERTEX_TYPE_FLOAT4:
                byteOffset += 16;
                break;
            case FOSTER_VERTEX_TYPE_BYTE4:
            case FOSTER_VERTEX_TYPE_UBYTE4:
                byteOffset += 4;
                break;
            case FOSTER_VERTEX_TYPE_SHORT2:
            case FOSTER_VERTEX_TYPE_USHORT2:
                byteOffset += 4;
                break;
            case FOSTER_VERTEX_TYPE_SHORT4:
            case FOSTER_VERTEX_TYPE_USHORT4:
                byteOffset += 8;
                break;

        }
    }

    void* blobPtr = ID3D10Blob_GetBufferPointer(shader->vertexBlob);
    SIZE_T blobSize = ID3D10Blob_GetBufferSize(shader->vertexBlob);
    ID3D11InputLayout* layout;
    HRESULT hr = ID3D11Device_CreateInputLayout(fdx.device, elements, format.elementCount,
                                                blobPtr, blobSize, &layout);
    SDL_free(elements);
    CHECK_RESULT(hr, "Failed to create input layout");

    if (group->count % FOSTER_GROWTH_BLOCK_SIZE == 0)
        group->instances = SDL_realloc(group->instances, sizeof(FosterLayoutInstance_D3D11) * (group->count + FOSTER_GROWTH_BLOCK_SIZE));

    group->instances[group->count].shader = shader;
    group->instances[group->count].layout = layout;
    group->count++;
    return layout;
}


void FosterRemoveLayoutInstances(FosterShader_D3D11* shader)
{
    for (int i = 0; i < fdx.layoutCache.count; i++)
    {
        for (int j = 0; j < fdx.layoutCache.layouts[i].count; j++)
        {
            if (fdx.layoutCache.layouts[i].instances[j].shader == shader)
            {
                ID3D11InputLayout_Release(fdx.layoutCache.layouts[i].instances[j].layout);
                for (int k = j; k < fdx.layoutCache.layouts[i].count - 1; k++)
                {
                    fdx.layoutCache.layouts[i].instances[k] = fdx.layoutCache.layouts[i].instances[k + 1];
                }
                fdx.layoutCache.layouts[i].count--;
            }
        }
    }
}

void FosterAddLayoutReference(FosterVertexFormat format)
{
    FosterLayoutGroup_D3D11* group = FosterGetLayoutGroup(format, NULL);
    if (group == NULL)
    {
        if (fdx.layoutCache.count % FOSTER_GROWTH_BLOCK_SIZE == 0)
            fdx.layoutCache.layouts = SDL_realloc(fdx.layoutCache.layouts, sizeof(FosterLayoutGroup_D3D11) * (fdx.layoutCache.count + FOSTER_GROWTH_BLOCK_SIZE));

        group = &fdx.layoutCache.layouts[fdx.layoutCache.count];
        group->format = format;
        group->instances = NULL;
        group->count = 0;
        group->references = 0;
        fdx.layoutCache.count++;
    }
    group->references++;
}

void FosterRemoveLayoutReference(FosterVertexFormat format)
{
    int index;
    FosterLayoutGroup_D3D11* group = FosterGetLayoutGroup(format, &index);
    if (group != NULL)
    {
        group->references--;
        if (group->references <= 0)
        {
            for (int i = 0; i < group->count; i++)
            {
                if (group->instances[i].layout)
                    ID3D11InputLayout_Release(group->instances[i].layout);
            }
            SDL_free(group->instances);

            if (index < fdx.layoutCache.count - 1 && fdx.layoutCache.count > 1)
                fdx.layoutCache.layouts[index] = fdx.layoutCache.layouts[fdx.layoutCache.count - 1];

            fdx.layoutCache.count--;
        }
    }
}

int FosterGetLayoutByteCount(FosterVertexFormat format)
{
    int byteCount = 0;
    for (int i = 0; i < format.elementCount; i++)
    {
        switch (format.elements[i].type)
        {
            case FOSTER_VERTEX_TYPE_FLOAT:
                byteCount += 4;
                break;
            case FOSTER_VERTEX_TYPE_FLOAT2:
                byteCount += 8;
                break;
            case FOSTER_VERTEX_TYPE_FLOAT3:
                byteCount += 12;
                break;
            case FOSTER_VERTEX_TYPE_FLOAT4:
                byteCount += 16;
                break;
            case FOSTER_VERTEX_TYPE_BYTE4:
            case FOSTER_VERTEX_TYPE_UBYTE4:
                byteCount += 4;
                break;
            case FOSTER_VERTEX_TYPE_SHORT2:
            case FOSTER_VERTEX_TYPE_USHORT2:
                byteCount += 4;
                break;
            case FOSTER_VERTEX_TYPE_SHORT4:
            case FOSTER_VERTEX_TYPE_USHORT4:
                byteCount += 8;
                break;
        }
    }
    return byteCount;
}

void FosterClearLayoutCache()
{
    for (int i = 0; i < fdx.layoutCache.count; i++)
    {
        for (int j = 0; j < fdx.layoutCache.layouts[i].count; j++)
        {
            ID3D11InputLayout_Release(fdx.layoutCache.layouts[i].instances[j].layout);
        }
        if (fdx.layoutCache.layouts[i].instances)
            SDL_free(fdx.layoutCache.layouts[i].instances);
    }
    if (fdx.layoutCache.layouts)
        SDL_free(fdx.layoutCache.layouts);
    fdx.layoutCache.count = 0;
}

void FosterBindMesh(FosterMesh_D3D11* mesh)
{
    if (fdx.stateInitializing || !FosterVertexFormatEqual(fdx.stateVertexFormat, mesh->vertexFormat))
    {
        ID3D11InputLayout* layout = FosterGetLayoutInstance(fdx.stateShader, mesh->vertexFormat);
        fdx.stateVertexFormat = mesh->vertexFormat;
        FosterLogInfo("Binding layout %p", layout);
        ID3D11DeviceContext_IASetInputLayout(fdx.context, layout);
    }
    if (fdx.stateInitializing || fdx.stateVertexBuffer != mesh->vertexBuffer)
    {
        fdx.stateVertexBuffer = mesh->vertexBuffer;
        UINT stride = mesh->vertexSize;
        UINT offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(fdx.context, 0, 1, &mesh->vertexBuffer, &stride, &offset);
    }
    if (fdx.stateInitializing || fdx.stateIndexBuffer != mesh->indexBuffer)
    {
        fdx.stateIndexBuffer = mesh->indexBuffer;
        ID3D11DeviceContext_IASetIndexBuffer(fdx.context, mesh->indexBuffer, mesh->indexFormat ==
            FOSTER_INDEX_FORMAT_THIRTY_TWO ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
    }
}

void FosterBindFrameBuffer_D3D11(FosterTarget_D3D11* target)
{
    ID3D11RenderTargetView** views = &fdx.backBufferView;
    int viewCount = 1;
    ID3D11DepthStencilView* depthView = fdx.backBufferDepthView;
    if(target != NULL)
    {
        views = target->colorViews;
        viewCount = target->colorViewCount;
        depthView = target->depthView;
        fdx.stateFrameBufferWidth = target->width;
        fdx.stateFrameBufferHeight = target->height;
    }
    else
    {
        FosterGetSizeInPixels(&fdx.stateFrameBufferWidth, &fdx.stateFrameBufferHeight);
    }
    if (fdx.stateInitializing || fdx.stateFrameBuffer != *views)
    {
        fdx.stateFrameBuffer = *views;
        ID3D11DeviceContext_OMSetRenderTargets(fdx.context, viewCount, views, depthView);
    }
}

void FosterSetTextureSampler_D3D11(FosterTexture_D3D11* tex, FosterTextureSampler sampler)
{
    if (!tex->disposed && (
            tex->sampler.filter != sampler.filter ||
            tex->sampler.wrapX != sampler.wrapX ||
            tex->sampler.wrapY != sampler.wrapY))
    {

    }
}

void FosterTextureReturnReference_D3D11(FosterTexture_D3D11* texture)
{
    if (texture != NULL)
    {
        texture->refCount--;
        if (texture->refCount <= 0)
        {
            if (!texture->disposed)
                FosterLogError("Texture is being free'd without deleting its GPU Texture Data");
            SDL_free(texture);
        }
    }
}

FosterTexture_D3D11* FosterTextureRequestReference_D3D11(FosterTexture_D3D11* texture)
{
    if (texture != NULL)
        texture->refCount++;
    return texture;
}

void FosterSetViewport_D3D11(int enabled, FosterRect rect)
{
    FosterRect viewport;

    if (enabled)
    {
        viewport = rect;
        viewport.y = fdx.stateFrameBufferHeight - viewport.y - viewport.h;
    }
    else
    {
        viewport.x = 0; viewport.y = 0;
        viewport.w = fdx.stateFrameBufferWidth;
        viewport.h = fdx.stateFrameBufferHeight;
    }

    if (fdx.stateInitializing || !FOSTER_RECT_EQUAL(viewport, fdx.stateViewport))
    {
        fdx.stateViewport = viewport;
        D3D11_VIEWPORT vp;
        vp.TopLeftX = (float)viewport.x;
        vp.TopLeftY = (float)viewport.y;
        vp.Width = (float)viewport.w;
        vp.Height = (float)viewport.h;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(fdx.context, 1, &vp);
    }
}

void FosterSetScissor_D3D11(int enabled, FosterRect rect)
{
    // get input scissor first
    FosterRect scissor = rect;
    scissor.y = fdx.stateFrameBufferHeight - scissor.y - scissor.h;
    if (scissor.w < 0) scissor.w = 0;
    if (scissor.h < 0) scissor.h = 0;

    // toggle scissor
    if (fdx.stateInitializing ||
        enabled && !FOSTER_RECT_EQUAL(scissor, fdx.stateScissor))
    {
        if (enabled)
            ID3D11DeviceContext_RSSetScissorRects(fdx.context, 1, (D3D11_RECT*)&scissor);
        else
            ID3D11DeviceContext_RSSetScissorRects(fdx.context, 0, NULL);
    }
}

void FosterSetBlend_D3D11(const FosterBlend* blend) {
    if (blend == NULL && fdx.stateBlendEnabled)
        return;

    if (fdx.stateInitializing
    || (blend != NULL) != fdx.stateBlendEnabled
    || SDL_memcmp(blend, &fdx.stateBlend, sizeof(FosterBlend)) != 0)
    {
        D3D11_BLEND_DESC desc;
        desc.AlphaToCoverageEnable = FALSE;
        desc.IndependentBlendEnable = FALSE;
        desc.RenderTarget[0].BlendEnable = fdx.stateBlendEnabled = blend != NULL;
        if(blend != NULL) {
            fdx.stateBlend = *blend;
            desc.RenderTarget[0].BlendOp = FosterBlendOpToD3D11(blend->colorOp);
            desc.RenderTarget[0].BlendOpAlpha = FosterBlendOpToD3D11(blend->alphaOp);
            desc.RenderTarget[0].DestBlend = FosterBlendFactorToD3D11(blend->colorDst);
            desc.RenderTarget[0].DestBlendAlpha = FosterBlendFactorToD3D11(blend->alphaDst);
            desc.RenderTarget[0].SrcBlend = FosterBlendFactorToD3D11(blend->colorSrc);
            desc.RenderTarget[0].SrcBlendAlpha = FosterBlendFactorToD3D11(blend->alphaSrc);

        }
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (fdx.blendState != NULL)
            ID3D11BlendState_Release(fdx.blendState);

        HRESULT hr = ID3D11Device_CreateBlendState(fdx.device, &desc, &fdx.blendState);
        CHECK_RESULT_V(hr, "Failed to create blend state");

        float factor[4] = {
                (float)(blend->rgba & 0xFF),
                (float)((blend->rgba >> 8) & 0xFF),
                (float)((blend->rgba >> 16) & 0xFF),
                (float)((blend->rgba >> 24) & 0xFF)
        };
        ID3D11DeviceContext_OMSetBlendState(fdx.context, fdx.blendState, factor, blend->mask);
    }
}

void FosterSetCompare_D3D11(FosterCompare compare)
{
    if (fdx.stateInitializing || compare != fdx.stateCompare) {
        D3D11_DEPTH_STENCIL_DESC desc;
        desc.DepthEnable = compare != FOSTER_COMPARE_NONE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        if (desc.DepthEnable)
        {
            switch (compare)
            {
                case FOSTER_COMPARE_NONE: break;
                case FOSTER_COMPARE_LESS:
                    desc.DepthFunc = D3D11_COMPARISON_LESS;
                    break;
                case FOSTER_COMPARE_EQUAL:
                    desc.DepthFunc = D3D11_COMPARISON_EQUAL;
                    break;
                case FOSTER_COMPARE_LESS_OR_EQUAL:
                    desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
                    break;
                case FOSTER_COMPARE_GREATER:
                    desc.DepthFunc = D3D11_COMPARISON_GREATER;
                    break;
                case FOSTER_COMPARE_NOT_EQUAL:
                    desc.DepthFunc = D3D11_COMPARISON_NOT_EQUAL;
                    break;
                case FOSTER_COMPARE_GREATOR_OR_EQUAL:
                    desc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
                    break;
                case FOSTER_COMPARE_ALWAYS:
                    desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
                    break;
                case FOSTER_COMPARE_NEVER:
                    desc.DepthFunc = D3D11_COMPARISON_NEVER;
                    break;
            }
        }

        // update state in fdx.depthState
        if (fdx.depthState != NULL)
            ID3D11DepthStencilState_Release(fdx.depthState);
        HRESULT hr = ID3D11Device_CreateDepthStencilState(fdx.device, &desc, &fdx.depthState);
        CHECK_RESULT_V(hr, "Failed to create depth stencil state");

        ID3D11DeviceContext_OMSetDepthStencilState(fdx.context, fdx.depthState, 0);


        fdx.stateCompare = compare;
    }
}

void FosterSetCull_D3D11(FosterCull cull)
{
    if (fdx.stateInitializing || cull != fdx.stateCull)
    {
        D3D11_RASTERIZER_DESC desc;
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = FosterCullModeToD3D11(cull);
        desc.FrontCounterClockwise = TRUE;
        desc.DepthBias = 0;
        desc.DepthBiasClamp = 0.0f;
        desc.SlopeScaledDepthBias = 0.0f;
        desc.DepthClipEnable = TRUE;
        desc.ScissorEnable = FALSE;
        desc.MultisampleEnable = FALSE;
        desc.AntialiasedLineEnable = FALSE;

        if (fdx.rasterizerState != NULL)
            ID3D11RasterizerState_Release(fdx.rasterizerState);
        HRESULT hr = ID3D11Device_CreateRasterizerState(fdx.device, &desc, &fdx.rasterizerState);
        CHECK_RESULT_V(hr, "Failed to create rasterizer state");

        ID3D11DeviceContext_RSSetState(fdx.context, fdx.rasterizerState);
        fdx.stateCull = cull;
    }
}

unsigned int FosterReflectUniforms_D3D11(FosterUniform_D3D11** dest, ID3D11ShaderReflection* reflector, int start_at, unsigned int* bytes)
{
    D3D11_SHADER_DESC shaderDesc;
    HRESULT hr = reflector->lpVtbl->GetDesc(reflector, &shaderDesc);
    CHECK_RESULT(hr, "Failed to get vertex shader description");

    unsigned int uniformCount = start_at;

    for(unsigned int i = 0; i < shaderDesc.BoundResources; i++)
    {
        D3D11_SHADER_INPUT_BIND_DESC bindDesc;
        hr = reflector->lpVtbl->GetResourceBindingDesc(reflector, i, &bindDesc);
        CHECK_RESULT(hr, "Failed to get resource binding description");

        if (bindDesc.Type == D3D_SIT_TEXTURE && bindDesc.Dimension == D3D_SRV_DIMENSION_TEXTURE2D)
        {
            *dest = SDL_realloc(*dest, sizeof(FosterUniform_D3D11) * (uniformCount + 1));
            FosterUniform_D3D11* info = &(*dest)[uniformCount];
            FosterLogInfo("registering texture %s at %d, idx=%i", bindDesc.Name, bindDesc.BindPoint, uniformCount);
            info->name = SDL_malloc(strlen(bindDesc.Name) + 1);
            strcpy(info->name, bindDesc.Name);
            info->offset = *bytes;
            *bytes += bindDesc.BindCount;
            info->index = uniformCount;
            info->size = bindDesc.BindCount;
            info->type = FOSTER_UNIFORM_TYPE_TEXTURE2D;
            uniformCount++;
        }
        else if (bindDesc.Type == D3D_SIT_SAMPLER)
        {
            *dest = SDL_realloc(*dest, sizeof(FosterUniform_D3D11) * (uniformCount + 1));
            FosterUniform_D3D11* info = &(*dest)[uniformCount];
            FosterLogInfo("registering sampler %s at %d, idx=%i", bindDesc.Name, bindDesc.BindPoint, uniformCount);
            info->name = SDL_malloc(strlen(bindDesc.Name) + 1);
            strcpy(info->name, bindDesc.Name);
            info->offset = *bytes;
            info->size = bindDesc.BindCount;
            *bytes += bindDesc.BindCount;
            info->index = uniformCount;
            info->type = FOSTER_UNIFORM_TYPE_SAMPLER2D;
            uniformCount++;
        }
    }

    FosterLogInfo("Uniforms: %d", shaderDesc.ConstantBuffers);
    for (unsigned int i = 0; i < shaderDesc.ConstantBuffers; i++)
    {
        ID3D11ShaderReflectionConstantBuffer* buffer = reflector->lpVtbl->GetConstantBufferByIndex(reflector, i);
        D3D11_SHADER_BUFFER_DESC bufferDesc;

        hr = buffer->lpVtbl->GetDesc(buffer, &bufferDesc);
        CHECK_RESULT(hr, "Failed to get constant buffer description");

        for (unsigned int j = 0; j < bufferDesc.Variables; j++)
        {
            ID3D11ShaderReflectionVariable* variable = buffer->lpVtbl->GetVariableByIndex(buffer, j);
            D3D11_SHADER_VARIABLE_DESC variableDesc;
            D3D11_SHADER_TYPE_DESC typeDesc;
            hr = variable->lpVtbl->GetDesc(variable, &variableDesc);
            CHECK_RESULT(hr, "Failed to get variable description");
            ID3D11ShaderReflectionType* type = variable->lpVtbl->GetType(variable);
            hr = type->lpVtbl->GetDesc(type, &typeDesc);
            CHECK_RESULT(hr, "Failed to get type description");
            FosterLogInfo("registering texture %s at %d, idx=%i", variableDesc.Name, variableDesc.StartOffset, uniformCount);

            *dest = SDL_realloc(*dest, sizeof(FosterUniform_D3D11) * (uniformCount + 1));
            FosterUniform_D3D11* info = &(*dest)[uniformCount];
            info->name = SDL_malloc(strlen(variableDesc.Name) + 1);
            strcpy(info->name, variableDesc.Name);
            info->offset = *bytes;
            info->size = variableDesc.Size;
            *bytes += variableDesc.Size;
            info->index = uniformCount;
            info->type = FosterUniformTypeFromD3D11(&typeDesc);
            uniformCount++;
        }
    }
    return uniformCount;
}

void FosterPrepare_D3D11()
{
    FosterState* state = FosterGetState();
}

bool FosterInitialize_D3D11() {
    HRESULT hr = S_OK;

    FosterState *state = FosterGetState();
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    CHECK(SDL_GetWindowWMInfo(state->window, &wmInfo), "Failed to get window info");
    CHECK(IsWindow(wmInfo.info.win.window), "Invalid window handle for D3D11");
    int width, height;
    FosterGetSize(&width, &height);

    // device creation
    UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED;
#if defined(DEBUG) || defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] =
    {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
    };

    DXGI_SWAP_CHAIN_DESC swapChainDesc;
    {
        swapChainDesc.BufferCount = 1;
        swapChainDesc.BufferDesc.Width = width;
        swapChainDesc.BufferDesc.Height = height;
        swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
        swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
        swapChainDesc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
        swapChainDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.OutputWindow = wmInfo.info.win.window;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.Windowed = true;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    };

    hr = D3D11CreateDeviceAndSwapChain(
            NULL,
            D3D_DRIVER_TYPE_HARDWARE,
            NULL,
            0,
            featureLevels,
            sizeof(featureLevels) / sizeof(featureLevels[0]),
            D3D11_SDK_VERSION,
            &swapChainDesc,
            &fdx.swapChain,
            &fdx.device,
            &fdx.featureLevel,
            &fdx.context
    );

    CHECK_RESULT(hr, "Failed to create a D3D11 device");

	// get the back buffer
	ID3D11Texture2D* frame_buffer;
	hr = IDXGISwapChain_GetBuffer(fdx.swapChain, 0, &IID_ID3D11Texture2D, (void**)&frame_buffer);
    CHECK_RESULT(hr, "Failed to get the back buffer");
    ID3D11Device_CreateRenderTargetView(fdx.device, (ID3D11Resource*)frame_buffer, NULL, &fdx.backBufferView);
    ID3D11Texture2D_Release(frame_buffer);

    // get maximums
    {
        fdx.max_texture_size = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        fdx.max_texture_image_units = 65535;
        fdx.max_renderbuffer_size = D3D11_REQ_RENDER_TO_BUFFER_WINDOW_WIDTH;
    }

    // initialize rest of state
    fdx.stateInitializing = 1;
    FosterRect zeroRect = { 0 };
    FosterBlend zeroBlend = { 0 };
    FosterBindFrameBuffer_D3D11(NULL);
    FosterSetBlend_D3D11(&zeroBlend);
    FosterSetViewport_D3D11(0, zeroRect);
    fdx.stateInitializing = 0;

	// log
	{
		IDXGIDevice* dxgi_device;
		IDXGIAdapter* dxgi_adapter;
		DXGI_ADAPTER_DESC adapter_desc;
	
		hr = ID3D11Device_QueryInterface(fdx.device, &IID_IDXGIDevice, (void**)&dxgi_device);

		if (SUCCEEDED(hr))
		{
			IDXGIDevice_GetAdapter(dxgi_device, &dxgi_adapter);
			IDXGIAdapter_GetDesc(dxgi_adapter, &adapter_desc);

			FosterLogInfo("DirectX 11: %ls", adapter_desc.Description);

			IDXGIDevice_Release(dxgi_device);
			IDXGIAdapter_Release(dxgi_adapter);
		}
		else
		{
			FosterLogInfo("DirectX 11: (No further information)");
		}
	}

	return true;
}

void FosterShutdown_D3D11()
{
    if (fdx.blendState != NULL)
        ID3D11BlendState_Release(fdx.blendState);
    if (fdx.backBufferView != NULL)
        ID3D11RenderTargetView_Release(fdx.backBufferView);
    if (fdx.backBufferDepthView != NULL)
        ID3D11DepthStencilView_Release(fdx.backBufferDepthView);
    if (fdx.swapChain != NULL)
        IDXGISwapChain_Release(fdx.swapChain);
    if (fdx.context != NULL)
        ID3D11DeviceContext_Release(fdx.context);
    if (fdx.device != NULL)
        ID3D11Device_Release(fdx.device);
}

void FosterFrameBegin_D3D11()
{

}

void FosterFrameEnd_D3D11()
{
    FosterState* state = FosterGetState();

    FosterBindFrameBuffer_D3D11(NULL);

    // present
    IDXGISwapChain_Present(fdx.swapChain, 1, 0);

    SDL_GL_SwapWindow(state->window);
}

FosterTexture* FosterTextureCreate_D3D11(int width, int height, FosterTextureFormat format)
{
    D3D11_TEXTURE2D_DESC desc;
    {
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
    };
    FosterTexture_D3D11* tex = NULL;

    if (width > fdx.max_texture_size || height > fdx.max_texture_size)
    {
        FosterLogError("Exceeded Max Texture Size of %i", fdx.max_texture_size);
        return NULL;
    }

    switch (format)
    {
    case FOSTER_TEXTURE_FORMAT_R8:
        desc.Format = DXGI_FORMAT_R8_UNORM;
        break;
    case FOSTER_TEXTURE_FORMAT_R8G8B8A8:
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case FOSTER_TEXTURE_FORMAT_DEPTH24_STENCIL8:
        desc.Format = DXGI_FORMAT_R16_UNORM;
        break;
    default:
        FosterLogError("Invalid Texture Format (%i)", format);
        return NULL;
    }
    ID3D11Texture2D *texture;
    HRESULT hr = ID3D11Device_CreateTexture2D(fdx.device, &desc, NULL, &texture);
    CHECK_RESULT_A(hr, "Failed to create Texture", NULL);

    ID3D11ShaderResourceView* view;
    hr = ID3D11Device_CreateShaderResourceView(fdx.device, (ID3D11Resource*)texture, NULL, &view);
    CHECK_RESULT_A(hr, "Failed to create Shader Resource View", NULL);

    tex = (FosterTexture_D3D11*)SDL_malloc(sizeof(FosterTexture_D3D11));
    tex->texture = texture;
    tex->view = view;
    tex->refCount = 1;
    tex->width = width;
    tex->height = height;
    tex->dxgiFormat = desc.Format;
    return (FosterTexture*)tex;
}

void FosterTextureSetData_D3D11(FosterTexture* texture, void* data, int length)
{
    FosterTexture_D3D11* tex = (FosterTexture_D3D11*)texture;
    ID3D11DeviceContext_UpdateSubresource(fdx.context,
        (ID3D11Resource*)tex->texture,
        0,
        NULL,
        data,
        tex->width * 4,
        0);
}

void FosterTextureGetData_D3D11(FosterTexture* texture, void* data, int length)
{
    // get texture data
    FosterTexture_D3D11* tex = (FosterTexture_D3D11*)texture;

    // TODO: implement
}

void FosterTextureDestroy_D3D11(FosterTexture* texture)
{
    FosterTexture_D3D11* tex = (FosterTexture_D3D11*)texture;
    if (!tex->disposed)
    {
        tex->disposed = 1;
        if (tex->view != NULL)
            ID3D11ShaderResourceView_Release(tex->view);
        if (tex->texture != NULL)
            ID3D11Texture2D_Release(tex->texture);
        FosterTextureReturnReference_D3D11(tex);
    }
}

FosterTarget* FosterTargetCreate_D3D11(int width, int height, FosterTextureFormat* attachments, int attachmentCount)
{
    FosterTarget_D3D11 result;
    FosterTarget_D3D11* target = NULL;
    HRESULT hr = S_OK;

    result.width = width;
    result.height = height;
    result.colorViewCount = attachmentCount;
    for (int i = 0; i < FOSTER_MAX_TARGET_ATTACHMENTS; i++)
    {
        result.colorViews[i] = NULL;
        result.colorTextures[i] = NULL;
    }

    // generate attachments
    for (int i = 0; i < attachmentCount; i++)
    {
        if (attachments[i] == FOSTER_TEXTURE_FORMAT_DEPTH24_STENCIL8)
        {
            if(result.depthView)
                FosterLogWarn("Multiple Depth Attachments are not supported, only the first will be used");
            else {
                result.depthTexture = (FosterTexture_D3D11*)FosterTextureCreate_D3D11(width, height, attachments[i]);
                hr = ID3D11Device_CreateDepthStencilView(fdx.device, (ID3D11Resource *) result.colorTextures[i]->texture,
                                                         NULL,
                                                         &result.depthView);
                if (!SUCCEEDED(hr)) {
                    FosterLogError("Failed to create depth attachment %i: result %#08X", i, hr);
                    FosterTextureDestroy_D3D11((FosterTexture *) result.depthTexture);
                    for (int j = 0; j < i; j++) {
                        FosterTextureDestroy_D3D11((FosterTexture *) result.colorTextures[j]);
                        ID3D11RenderTargetView_Release(result.colorViews[j]);
                    }
                    return NULL;
                }
            }
        }
        else
        {
            result.depthTexture = (FosterTexture_D3D11*)FosterTextureCreate_D3D11(width, height, attachments[i]);
            hr = ID3D11Device_CreateRenderTargetView(fdx.device, (ID3D11Resource *) result.colorTextures[i]->texture, NULL,
                                                     &result.colorViews[i]);
            if (!SUCCEEDED(hr)) {
                FosterLogError("Failed to create attachment %i: result %#08X", i, hr);
                FosterTextureDestroy_D3D11((FosterTexture *) result.colorTextures[i]);
                for (int j = 0; j < i; j++) {
                    FosterTextureDestroy_D3D11((FosterTexture *) result.colorTextures[j]);
                    ID3D11RenderTargetView_Release(result.colorViews[j]);
                }
                return NULL;
            }
        }
    }

    target = (FosterTarget_D3D11*)SDL_malloc(sizeof(FosterTarget_D3D11));
    *target = result;
    return (FosterTarget*)target;
}

FosterTexture* FosterTargetGetAttachment_D3D11(FosterTarget* target, int index)
{
    FosterTarget_D3D11* tar = (FosterTarget_D3D11*)target;
#if defined(DEBUG) || defined(_DEBUG)
    if (index < 0 || index >= tar->colorViewCount)
    {
        FosterLogError("Invalid Attachment Index: %i", index);
        return NULL;
    }
#endif
    return (FosterTexture*)tar->colorTextures[index];
}

void FosterTargetDestroy_D3D11(FosterTarget* target)
{
    FosterTarget_D3D11* tar = (FosterTarget_D3D11*)target;
    for (int i = 0; i < tar->colorViewCount; i++)
    {
        FosterTextureDestroy_D3D11((FosterTexture*)tar->colorTextures[i]);
        ID3D11RenderTargetView_Release(tar->colorViews[i]);
    }
    if (tar->depthView != NULL)
        ID3D11DepthStencilView_Release(tar->depthView);
    SDL_free(tar);
}

FosterShader* FosterShaderCreate_D3D11(FosterShaderData* data)
{
    FosterShader_D3D11 result;
    FosterShader_D3D11* shader = NULL;
    ID3DBlob* errorBlob;
    ID3DBlob* vertexBlob;
    ID3DBlob* pixelBlob;
    HRESULT hr = S_OK;

    if (data->vertexShader == NULL)
    {
        FosterLogError("Invalid Vertex Shader");
        return NULL;
    }

    if (data->fragmentShader == NULL)
    {
        FosterLogError("Invalid Fragment Shader");
        return NULL;
    }

    // compile shaders
    {
        // vertex
        hr = D3DCompile(data->vertexShader,
                        strlen(data->vertexShader),
                        NULL,
                        NULL,
                        NULL,
                        "main",
                        FOSTER_VERTEX_SHADER_MODEL,
                        0,
                        0,
                        &vertexBlob,
                        &errorBlob);
        if (FAILED(hr))
        {
            FosterLogError("Failed to compile vertex shader: %s", ID3D10Blob_GetBufferPointer(errorBlob));
            ID3D10Blob_Release(errorBlob);
            return NULL;
        }

        // pixel
        hr = D3DCompile(data->fragmentShader,
                        strlen(data->fragmentShader),
                        NULL,
                        NULL,
                        NULL,
                        "main",
                        FOSTER_PIXEL_SHADER_MODEL,
                        0,
                        0,
                        &pixelBlob,
                        &errorBlob);

        if (FAILED(hr))
        {
            FosterLogError("Failed to compile pixel shader: %s", ID3D10Blob_GetBufferPointer(errorBlob));
            ID3D10Blob_Release(errorBlob);
            return NULL;
        }
    }

    // create shader
    {
        hr = ID3D11Device_CreateVertexShader(fdx.device, ID3D10Blob_GetBufferPointer(vertexBlob),
                                             ID3D10Blob_GetBufferSize(vertexBlob), NULL, &result.vertexShader);
        CHECK_RESULT(hr, "Failed to create vertex shader");

        hr = ID3D11Device_CreatePixelShader(fdx.device, ID3D10Blob_GetBufferPointer(pixelBlob),
                                            ID3D10Blob_GetBufferSize(pixelBlob), NULL, &result.pixelShader);
        CHECK_RESULT(hr, "Failed to create pixel shader");
    }

    // load uniforms into result.uniforms / result.uniformCount
    unsigned int uniformBytes = 0;
    {
        // count
        ID3D11ShaderReflection* reflectionVertex;
        hr = D3DReflect(ID3D10Blob_GetBufferPointer(vertexBlob), ID3D10Blob_GetBufferSize(vertexBlob),
                        &IID_ID3D11ShaderReflection, (void**)&reflectionVertex);
        CHECK_RESULT(hr, "Failed to reflect vertex shader");
        ID3D11ShaderReflection* reflectionPixel;
        hr = D3DReflect(ID3D10Blob_GetBufferPointer(pixelBlob), ID3D10Blob_GetBufferSize(pixelBlob),
                        &IID_ID3D11ShaderReflection, (void**)&reflectionPixel);
        CHECK_RESULT(hr, "Failed to reflect pixel shader");


        result.uniforms = NULL;
        result.uniformCount = FosterReflectUniforms_D3D11(&result.uniforms, reflectionVertex, 0, &uniformBytes);
        result.uniformCount = FosterReflectUniforms_D3D11(&result.uniforms, reflectionPixel, result.uniformCount, &uniformBytes);

        // get input semantics
        D3D11_SHADER_DESC desc;
        hr = reflectionVertex->lpVtbl->GetDesc(reflectionVertex, &desc);
        CHECK_RESULT(hr, "Failed to get vertex shader description");
        result.inputCount = desc.InputParameters;
        result.inputs = SDL_malloc(sizeof(FosterShaderInput_D3D11) * result.inputCount);
        for (unsigned int i = 0; i < desc.InputParameters; i++)
        {
            D3D11_SIGNATURE_PARAMETER_DESC param;
            hr = reflectionVertex->lpVtbl->GetInputParameterDesc(reflectionVertex, i, &param);
            CHECK_RESULT(hr, "Failed to get input parameter description");
            result.inputs[i].name = SDL_malloc(strlen(param.SemanticName) + 1);
            strcpy(result.inputs[i].name, param.SemanticName);
            result.inputs[i].index = param.SemanticIndex;
        }

        reflectionVertex->lpVtbl->Release(reflectionVertex);
        reflectionPixel->lpVtbl->Release(reflectionPixel);
    }

    // initialize uniform buffer
    {
        if (uniformBytes > 0) {
            D3D11_BUFFER_DESC desc;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.ByteWidth = (uniformBytes + 15) & ~15;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            desc.MiscFlags = 0;
            desc.StructureByteStride = 0;
            hr = ID3D11Device_CreateBuffer(fdx.device, &desc, NULL, &result.uniformBuffer);
            CHECK_RESULT(hr, "Failed to create uniform buffer");
        }
    }

    result.vertexBlob = vertexBlob;
    ID3D10Blob_Release(pixelBlob);

    for (int i = 0; i < FOSTER_MAX_UNIFORM_TEXTURES; i++)
    {
        result.textures[i] = NULL;
        result.samplers[i] = NULL;
    }

    shader = (FosterShader_D3D11*)SDL_malloc(sizeof(FosterShader_D3D11));
    *shader = result;
    return (FosterShader*)shader;
}

void FosterShaderSetUniform_D3D11(FosterShader* shader, int index, float* values) {
    FosterShader_D3D11* sh = (FosterShader_D3D11*)shader;

    // Check if the index is within the range of the shader's uniforms
    if (index < 0 || index >= sh->uniformCount) {
        FosterLogError("Invalid Uniform Index: %i", index);
        return;
    }

    // set uniform
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        ID3D11DeviceContext_Map(fdx.context, (ID3D11Resource*)sh->uniformBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        char* dest = (char*)mapped.pData + sh->uniforms[index].offset;
        SDL_memcpy(dest, values, sh->uniforms[index].size);
        // print out data hex for debugging
        ID3D11DeviceContext_Unmap(fdx.context, (ID3D11Resource*)sh->uniformBuffer, 0);
    }
}

void FosterShaderSetTexture_D3D11(FosterShader* shader, int index, FosterTexture** values)
{
    FosterShader_D3D11* it = (FosterShader_D3D11*)shader;

    if (index < 0 || index >= it->uniformCount)
    {
        FosterLogError("Invalid Texture Index: %i", index);
        return;
    }

    FosterUniform_D3D11* uniform = &it->uniforms[index];
    if (uniform->type != FOSTER_UNIFORM_TYPE_TEXTURE2D)
    {
        FosterLogError("Uniform at index %i is not a texture (%p)", index, uniform->type);
        return;
    }

    for (unsigned int i = 0; i < uniform->size; i++)
    {
        if (values[i] != NULL)
        {
            FosterTexture_D3D11* tex = (FosterTexture_D3D11*)values[i];
            FosterTextureReturnReference_D3D11(it->textures[i]);
            it->textures[i] = FosterTextureRequestReference_D3D11(tex);
        }
    }
}

void FosterShaderSetSampler_D3D11(FosterShader* shader, int index, FosterTextureSampler* values)
{
    FosterShader_D3D11* it = (FosterShader_D3D11*)shader;

    if (index < 0 || index >= it->uniformCount)
    {
        FosterLogError("Invalid Sampler Index: %i", index);
        return;
    }


    FosterUniform_D3D11* uniform = &it->uniforms[index];
    if (uniform->type != FOSTER_UNIFORM_TYPE_SAMPLER2D)
    {
        FosterLogError("Uniform at index %i is not a sampler", index);
        return;
    }

    // set sampler
    for (unsigned int i = 0; i < uniform->size; i++)
    {
        if (it->samplers[i] == NULL) {
            it->samplers[i] = (FosterSampler_D3D11 *) SDL_malloc(sizeof(FosterSampler_D3D11));
            it->samplers[i]->sampler = NULL;
            it->samplers[i]->settings = NULL;
        }
        if (it->samplers[i]->settings == NULL && values == NULL)
            continue;

        D3D11_SAMPLER_DESC desc = {0};
        if (values != NULL) {
            if (it->samplers[i]->settings)
                if (SDL_memcmp(it->samplers[i]->settings, values, sizeof(FosterTextureSampler)) == 0)
                    continue;

            // Convert the Foster sampler to a D3D11 sampler
            desc.Filter = FosterFilterToD3D11(values[i].filter);
            desc.AddressU = FosterWrapToD3D11(values[i].wrapX);
            desc.AddressV = FosterWrapToD3D11(values[i].wrapY);
            desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.BorderColor[0] = 1.0f;
            desc.BorderColor[1] = 1.0f;
            desc.BorderColor[2] = 1.0f;
            desc.BorderColor[3] = 1.0f;
            desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        }
        else if (it->samplers[i]->settings)
        {
            SDL_free(it->samplers[i]->settings);
            it->samplers[i]->settings = NULL;
        }


        // Create the sampler
        ID3D11SamplerState* sampler;
        HRESULT hr = ID3D11Device_CreateSamplerState(fdx.device, &desc, &sampler);
        CHECK_RESULT_V(hr, "Failed to create sampler state");

        // Set the sampler
        if (it->samplers[i]->sampler != NULL)
            ID3D11SamplerState_Release(it->samplers[i]->sampler);
        it->samplers[i]->sampler = sampler;

        if (values != NULL)
        {
            it->samplers[i]->settings = SDL_malloc(sizeof(FosterTextureSampler));
            memcpy(it->samplers[i]->settings, values, sizeof(FosterTextureSampler));
        }
    }
}

void FosterShaderGetUniforms_D3D11(FosterShader* shader, FosterUniformInfo* output, int* count, int max)
{
    FosterShader_D3D11* it = (FosterShader_D3D11*)shader;
    if (max > it->uniformCount)
        max = it->uniformCount;
    for (int i = 0; i < max; i++)
    {
        FosterUniform_D3D11* uniform = it->uniforms + i;
        output[i].index = (int)uniform->index;
        output[i].name = uniform->name;
        output[i].type = uniform->type;
        output[i].arrayElements = (int)uniform->size;
    }
    *count = max;
}

void FosterShaderDestroy_D3D11(FosterShader* shader)
{
    FosterShader_D3D11* sh = (FosterShader_D3D11*)shader;
    ID3D11VertexShader_Release(sh->vertexShader);
    ID3D11PixelShader_Release(sh->pixelShader);

    for (int i = 0; i < FOSTER_MAX_UNIFORM_TEXTURES; i++)
    {
        FosterTextureReturnReference_D3D11(sh->textures[i]);
    }

    for (int i = 0; i < sh->uniformCount; i++)
    {
        SDL_free(sh->uniforms[i].name);
    }

    for (unsigned int i = 0; i < sh->inputCount; i++)
    {
        SDL_free(sh->inputs[i].name);
    }

    for (int i = 0; i < FOSTER_MAX_UNIFORM_TEXTURES; i++)
    {
        if(sh->samplers[i] == NULL)
            continue;
        if (sh->samplers[i]->settings)
            SDL_free(sh->samplers[i]->settings);
        if (sh->samplers[i]->sampler)
            ID3D11SamplerState_Release(sh->samplers[i]->sampler);
        SDL_free(sh->samplers[i]);
    }

    SDL_free(sh->inputs);
    SDL_free(sh->uniforms);
    SDL_free(sh);
}

FosterMesh* FosterMeshCreate_D3D11()
{
    FosterMesh_D3D11 result = { 0 };
    FosterMesh_D3D11* mesh = NULL;

    result.vertexBuffer = NULL;
    result.indexBuffer = NULL;

    mesh = (FosterMesh_D3D11*)SDL_malloc(sizeof(FosterMesh_D3D11));
    *mesh = result;
    return (FosterMesh*)mesh;
}

void FosterMeshSetVertexFormat_D3D11(FosterMesh* mesh, FosterVertexFormat* format)
{
    // set vertex format
    FosterMesh_D3D11* m = (FosterMesh_D3D11*)mesh;
    // copy vertex format elements
    if (m->vertexFormat.elements != NULL)
        SDL_free(m->vertexFormat.elements);
    m->vertexFormat = *format;
    m->vertexFormat.elements = (FosterVertexFormatElement*)SDL_malloc(sizeof(FosterVertexFormatElement) * format->elementCount);
    memcpy(m->vertexFormat.elements, format->elements, sizeof(FosterVertexFormatElement) * format->elementCount);



    if (m->vertexBuffer == NULL)
    {
        D3D11_BUFFER_DESC desc;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.ByteWidth = format->stride;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        desc.StructureByteStride = 0;
        HRESULT hr = ID3D11Device_CreateBuffer(fdx.device, &desc, NULL, &m->vertexBuffer);
        CHECK_RESULT_V(hr, "Failed to create vertex buffer");
    }

    if (m->vertexFormat.elementCount != 0)
        FosterRemoveLayoutReference(m->vertexFormat);
    FosterAddLayoutReference(m->vertexFormat);
    m->vertexSize = FosterGetLayoutByteCount(m->vertexFormat);
}

void FosterMeshSetVertexData_D3D11(FosterMesh* mesh, void* data, int dataSize, int dataDestOffset)
{
    FosterMesh_D3D11* m = (FosterMesh_D3D11*)mesh;
    int trueSize = ((dataSize + dataDestOffset) + 15) & ~15;
    if (trueSize > m->vertexBytes || m->vertexBuffer == NULL)
    {
        if (m->vertexBuffer != NULL)
            ID3D11Buffer_Release(m->vertexBuffer);
        D3D11_BUFFER_DESC desc;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.ByteWidth = trueSize;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = 0;
        desc.StructureByteStride = 2;

        D3D11_SUBRESOURCE_DATA subData;
        subData.pSysMem = data;
        ID3D11Buffer* newBuffer;
        HRESULT hr = ID3D11Device_CreateBuffer(fdx.device, &desc, &subData, &newBuffer);
        CHECK_RESULT_V(hr, "Failed to create vertex buffer");
        m->vertexBuffer = newBuffer;
        m->vertexBytes = trueSize;
    }
    else
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        ID3D11DeviceContext_Map(fdx.context, (ID3D11Resource*)m->vertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        memcpy((char*)mapped.pData + dataDestOffset, data, dataSize);
        ID3D11DeviceContext_Unmap(fdx.context, (ID3D11Resource*)m->vertexBuffer, 0);
    }
}


void FosterMeshSetIndexFormat_D3D11(FosterMesh* mesh, FosterIndexFormat format)
{
    FosterMesh_D3D11* m = (FosterMesh_D3D11*)mesh;
    m->indexFormat = format;
}

void FosterMeshSetIndexData_D3D11(FosterMesh* mesh, void* data, int dataSize, int dataDestOffset)
{
    FosterMesh_D3D11* m = (FosterMesh_D3D11*)mesh;
    int trueSize = (dataSize + 15) & ~15;
    if (trueSize != m->indexBytes || m->indexBuffer == NULL)
    {
        if (m->indexBuffer != NULL)
            ID3D11Buffer_Release(m->indexBuffer);
        D3D11_BUFFER_DESC desc = { 0 };
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.ByteWidth = trueSize;
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        D3D11_SUBRESOURCE_DATA subData = { 0 };
        subData.pSysMem = data;

        ID3D11Buffer* newBuffer;
        HRESULT hr = ID3D11Device_CreateBuffer(fdx.device, &desc, &subData, &newBuffer);
        CHECK_RESULT_V(hr, "Failed to create index buffer");

        m->indexBuffer = newBuffer;
        m->indexBytes = trueSize;
    }
    else
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        ID3D11DeviceContext_Map(fdx.context, (ID3D11Resource*)m->indexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        memcpy(mapped.pData, data, trueSize);
        ID3D11DeviceContext_Unmap(fdx.context, (ID3D11Resource*)m->indexBuffer, 0);
    }
}

void FosterMeshDestroy_D3D11(FosterMesh* mesh)
{
    FosterMesh_D3D11* m = (FosterMesh_D3D11*)mesh;

    if (m->vertexBuffer != NULL)
        ID3D11Buffer_Release(m->vertexBuffer);
    if (m->indexBuffer != NULL)
        ID3D11Buffer_Release(m->indexBuffer);
    if (m->vertexFormat.elementCount != 0)
        FosterRemoveLayoutReference(m->vertexFormat);

    SDL_free(m);
}

void FosterDraw_D3D11(FosterDrawCommand* command)
{
    FosterTarget_D3D11* target = (FosterTarget_D3D11*)command->target;
    FosterShader_D3D11* shader = (FosterShader_D3D11*)command->shader;
    FosterMesh_D3D11* mesh = (FosterMesh_D3D11*)command->mesh;

    // set state
    FosterSetViewport_D3D11(command->hasViewport, command->viewport);
    FosterSetScissor_D3D11(command->hasScissor, command->scissor);
    FosterBindFrameBuffer_D3D11(target);
    ID3D11DeviceContext_IASetPrimitiveTopology(fdx.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    FosterBindShader_D3D11(shader);
    FosterBindMesh(mesh);
    FosterSetBlend_D3D11(&command->blend);
    FosterSetCompare_D3D11(command->compare);
    FosterSetCull_D3D11(command->cull);


    // draw the mesh
    {
        UINT indexStartPtr = 0;

        if (command->instanceCount > 0)
        {
            ID3D11DeviceContext_DrawIndexedInstanced(fdx.context, command->indexCount, command->instanceCount,
                                                     indexStartPtr, 0, 0);
        }
        else
        {
            ID3D11DeviceContext_DrawIndexed(fdx.context, command->indexCount, indexStartPtr, 0);
        }
    }
}

void FosterClear_D3D11(FosterClearCommand* command) {
    FosterSetViewport_D3D11(1, command->clip);
    FosterSetScissor_D3D11(0, fdx.stateScissor);

    ID3D11RenderTargetView **view;
    ID3D11DepthStencilView *depthView;
    int viewCount = 0;
    if (command->target)
    {
        view = ((FosterTarget_D3D11 *) command->target)->colorViews;
        viewCount = ((FosterTarget_D3D11 *) command->target)->colorViewCount;
        depthView = ((FosterTarget_D3D11 *) command->target)->depthView;
    }
    else
    {
        view = &fdx.backBufferView;
        viewCount = 1;
        depthView = fdx.backBufferDepthView;
    }

    if ((command->mask & FOSTER_CLEAR_MASK_COLOR) == FOSTER_CLEAR_MASK_COLOR)
    {
        float color[4] = {
                (float) (command->color.r) / 255.0f,
                (float) (command->color.g) / 255.0f,
                (float) (command->color.b) / 255.0f,
                (float) (command->color.a) / 255.0f
        };

        for (int i = 0; i < viewCount; i++) {
            ID3D11DeviceContext_ClearRenderTargetView(fdx.context, view[i], color);
        }
    }

    if ((command->mask & FOSTER_CLEAR_MASK_DEPTH) == FOSTER_CLEAR_MASK_DEPTH)
    {
        ID3D11DeviceContext_ClearDepthStencilView(fdx.context, depthView, D3D11_CLEAR_DEPTH, command->depth, 0);
    }

    if ((command->mask & FOSTER_CLEAR_MASK_DEPTH) == FOSTER_CLEAR_MASK_DEPTH)
    {
        ID3D11DeviceContext_ClearDepthStencilView(fdx.context, depthView, D3D11_CLEAR_STENCIL, 0, command->stencil);
    }
}

bool FosterGetDevice_D3D11(FosterRenderDevice* device)
{
	device->renderer = FOSTER_RENDERER_D3D11;
    device->prepare = FosterPrepare_D3D11;
    device->initialize = FosterInitialize_D3D11;
	device->shutdown = FosterShutdown_D3D11;
    device->frameBegin = FosterFrameBegin_D3D11;
    device->frameEnd = FosterFrameEnd_D3D11;
    device->textureCreate = FosterTextureCreate_D3D11;
    device->textureSetData = FosterTextureSetData_D3D11;
    device->textureGetData = FosterTextureGetData_D3D11;
    device->textureDestroy = FosterTextureDestroy_D3D11;
    device->targetCreate = FosterTargetCreate_D3D11;
    device->targetGetAttachment = FosterTargetGetAttachment_D3D11;
    device->targetDestroy = FosterTargetDestroy_D3D11;
    device->shaderCreate = FosterShaderCreate_D3D11;
    device->shaderSetUniform = FosterShaderSetUniform_D3D11;
    device->shaderSetTexture = FosterShaderSetTexture_D3D11;
    device->shaderSetSampler = FosterShaderSetSampler_D3D11;
    device->shaderGetUniforms = FosterShaderGetUniforms_D3D11;
    device->shaderDestroy = FosterShaderDestroy_D3D11;
    device->meshCreate = FosterMeshCreate_D3D11;
    device->meshSetVertexFormat = FosterMeshSetVertexFormat_D3D11;
    device->meshSetVertexData = FosterMeshSetVertexData_D3D11;
    device->meshSetIndexFormat = FosterMeshSetIndexFormat_D3D11;
    device->meshSetIndexData = FosterMeshSetIndexData_D3D11;
    device->meshDestroy = FosterMeshDestroy_D3D11;
    device->draw = FosterDraw_D3D11;
    device->clear = FosterClear_D3D11;
	return true;
}


#else // FOSTER_D3D11_ENABLED

#include "foster_renderer.h"

bool FosterGetDevice_D3D11(FosterRenderDevice* device)
{
	device->renderer = FOSTER_RENDERER_D3D11;
	return false;
}

#endif
