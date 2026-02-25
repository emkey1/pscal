#include "backend_ast/graphics_3d_metal_apple.h"

#if (defined(PSCAL_TARGET_IOS) || defined(__APPLE__)) && defined(SDL)

#include "core/sdl_headers.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <stdint.h>

#ifndef GL_NEVER
#define GL_NEVER 0x0200
#endif
#ifndef GL_LESS
#define GL_LESS 0x0201
#endif
#ifndef GL_EQUAL
#define GL_EQUAL 0x0202
#endif
#ifndef GL_LEQUAL
#define GL_LEQUAL 0x0203
#endif
#ifndef GL_GREATER
#define GL_GREATER 0x0204
#endif
#ifndef GL_NOTEQUAL
#define GL_NOTEQUAL 0x0205
#endif
#ifndef GL_GEQUAL
#define GL_GEQUAL 0x0206
#endif
#ifndef GL_ALWAYS
#define GL_ALWAYS 0x0207
#endif
#ifndef GL_SRC_ALPHA
#define GL_SRC_ALPHA 0x0302
#endif
#ifndef GL_ONE_MINUS_SRC_ALPHA
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#endif
#ifndef GL_ONE
#define GL_ONE 1
#endif

typedef struct {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLLibrary> library;
    id<MTLRenderPipelineState> triPipelines[3];
    id<MTLRenderPipelineState> linePipelines[3];
    id<MTLDepthStencilState> depthStates[2][8];
    CAMetalLayer* layer;
    struct SDL_Renderer* renderer;
    id<MTLTexture> depthTexture;
    id<CAMetalDrawable> drawable;
    id<MTLCommandBuffer> commandBuffer;
    id<MTLRenderCommandEncoder> encoder;
    int viewportX;
    int viewportY;
    int viewportWidth;
    int viewportHeight;
    bool frameActive;
    id<MTLBuffer> dynamicVertexBuffers[3];
    NSUInteger dynamicVertexBufferSize;
    NSUInteger dynamicVertexBufferOffset;
    int dynamicVertexBufferIndex;
} PscalMetal3DContext;

static PscalMetal3DContext gMetal3D = {0};
static const NSUInteger kPscalMetalInitialDynamicVertexBytes = 8u * 1024u * 1024u;

static NSString* pscalMetalShaderSource(void) {
    return @"#include <metal_stdlib>\n"
           "using namespace metal;\n"
           "struct VertexIn {\n"
           "    float3 position [[attribute(0)]];\n"
           "    float4 color [[attribute(1)]];\n"
           "};\n"
           "struct VertexOut {\n"
           "    float4 position [[position]];\n"
           "    float4 color;\n"
           "};\n"
           "vertex VertexOut pscalVertexMain(VertexIn vin [[stage_in]]) {\n"
           "    VertexOut out;\n"
           "    out.position = float4(vin.position.x, vin.position.y, vin.position.z, 1.0);\n"
           "    out.color = vin.color;\n"
           "    return out;\n"
           "}\n"
           "fragment float4 pscalFragmentMain(VertexOut inFrag [[stage_in]]) {\n"
           "    return inFrag.color;\n"
           "}\n";
}

static MTLCompareFunction pscalMetalDepthFunc(unsigned int depthFunc) {
    switch (depthFunc) {
        case GL_NEVER:
            return MTLCompareFunctionNever;
        case GL_LESS:
            return MTLCompareFunctionLess;
        case GL_EQUAL:
            return MTLCompareFunctionEqual;
        case GL_LEQUAL:
            return MTLCompareFunctionLessEqual;
        case GL_GREATER:
            return MTLCompareFunctionGreater;
        case GL_NOTEQUAL:
            return MTLCompareFunctionNotEqual;
        case GL_GEQUAL:
            return MTLCompareFunctionGreaterEqual;
        case GL_ALWAYS:
        default:
            return MTLCompareFunctionAlways;
    }
}

static int pscalMetalDepthFuncIndex(unsigned int depthFunc) {
    switch (depthFunc) {
        case GL_NEVER:
            return 0;
        case GL_LESS:
            return 1;
        case GL_EQUAL:
            return 2;
        case GL_LEQUAL:
            return 3;
        case GL_GREATER:
            return 4;
        case GL_NOTEQUAL:
            return 5;
        case GL_GEQUAL:
            return 6;
        case GL_ALWAYS:
        default:
            return 7;
    }
}

static int pscalMetalBlendIndex(bool blendEnabled, unsigned int blendSrc, unsigned int blendDst) {
    if (!blendEnabled) {
        return 0;
    }
    if (blendSrc == GL_SRC_ALPHA && blendDst == GL_ONE_MINUS_SRC_ALPHA) {
        return 1;
    }
    if (blendSrc == GL_SRC_ALPHA && blendDst == GL_ONE) {
        return 2;
    }
    return 0;
}

static id<MTLDepthStencilState> pscalMetalDepthState(bool depthTestEnabled,
                                                     bool depthWriteEnabled,
                                                     unsigned int depthFunc) {
    bool writes = depthTestEnabled && depthWriteEnabled;
    int writeIndex = writes ? 1 : 0;
    int funcIndex = depthTestEnabled ? pscalMetalDepthFuncIndex(depthFunc) : 7;
    id<MTLDepthStencilState> state = gMetal3D.depthStates[writeIndex][funcIndex];
    if (state) {
        return state;
    }

    MTLDepthStencilDescriptor* desc = [[MTLDepthStencilDescriptor alloc] init];
    desc.depthCompareFunction = depthTestEnabled
                                    ? pscalMetalDepthFunc(depthFunc)
                                    : MTLCompareFunctionAlways;
    desc.depthWriteEnabled = writes;
    state = [gMetal3D.device newDepthStencilStateWithDescriptor:desc];
    gMetal3D.depthStates[writeIndex][funcIndex] = state;
    return state;
}

static bool pscalMetalEnsureDepthTexture(void) {
    if (gMetal3D.viewportWidth <= 0 || gMetal3D.viewportHeight <= 0) {
        return false;
    }
    if (gMetal3D.depthTexture &&
        (int)gMetal3D.depthTexture.width == gMetal3D.viewportWidth &&
        (int)gMetal3D.depthTexture.height == gMetal3D.viewportHeight) {
        return true;
    }

    MTLTextureDescriptor* depthDesc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                           width:(NSUInteger)gMetal3D.viewportWidth
                                                          height:(NSUInteger)gMetal3D.viewportHeight
                                                       mipmapped:NO];
    depthDesc.storageMode = MTLStorageModePrivate;
    depthDesc.usage = MTLTextureUsageRenderTarget;
    gMetal3D.depthTexture = [gMetal3D.device newTextureWithDescriptor:depthDesc];
    return gMetal3D.depthTexture != nil;
}

static id<MTLRenderPipelineState> pscalMetalCreatePipeline(MTLPrimitiveType primitiveType,
                                                           int blendIndex) {
    (void)primitiveType;

    MTLVertexDescriptor* vertexDesc = [[MTLVertexDescriptor alloc] init];
    vertexDesc.attributes[0].format = MTLVertexFormatFloat3;
    vertexDesc.attributes[0].offset = 0;
    vertexDesc.attributes[0].bufferIndex = 0;
    vertexDesc.attributes[1].format = MTLVertexFormatFloat4;
    vertexDesc.attributes[1].offset = sizeof(float) * 3;
    vertexDesc.attributes[1].bufferIndex = 0;
    vertexDesc.layouts[0].stride = sizeof(PscalMetalVertex);
    vertexDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = [gMetal3D.library newFunctionWithName:@"pscalVertexMain"];
    desc.fragmentFunction = [gMetal3D.library newFunctionWithName:@"pscalFragmentMain"];
    desc.vertexDescriptor = vertexDesc;
    desc.colorAttachments[0].pixelFormat = gMetal3D.layer.pixelFormat;
    desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    desc.stencilAttachmentPixelFormat = MTLPixelFormatInvalid;

    MTLRenderPipelineColorAttachmentDescriptor* color0 = desc.colorAttachments[0];
    if (blendIndex == 1) {
        color0.blendingEnabled = YES;
        color0.rgbBlendOperation = MTLBlendOperationAdd;
        color0.alphaBlendOperation = MTLBlendOperationAdd;
        color0.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        color0.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        color0.sourceAlphaBlendFactor = MTLBlendFactorOne;
        color0.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    } else if (blendIndex == 2) {
        color0.blendingEnabled = YES;
        color0.rgbBlendOperation = MTLBlendOperationAdd;
        color0.alphaBlendOperation = MTLBlendOperationAdd;
        color0.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        color0.destinationRGBBlendFactor = MTLBlendFactorOne;
        color0.sourceAlphaBlendFactor = MTLBlendFactorOne;
        color0.destinationAlphaBlendFactor = MTLBlendFactorOne;
    } else {
        color0.blendingEnabled = NO;
    }

    NSError* error = nil;
    id<MTLRenderPipelineState> state = [gMetal3D.device newRenderPipelineStateWithDescriptor:desc
                                                                                         error:&error];
    return state;
}

static bool pscalMetalEnsurePipelines(void) {
    for (int blend = 0; blend < 3; ++blend) {
        if (!gMetal3D.triPipelines[blend]) {
            gMetal3D.triPipelines[blend] = pscalMetalCreatePipeline(MTLPrimitiveTypeTriangle, blend);
            if (!gMetal3D.triPipelines[blend]) {
                return false;
            }
        }
        if (!gMetal3D.linePipelines[blend]) {
            gMetal3D.linePipelines[blend] = pscalMetalCreatePipeline(MTLPrimitiveTypeLine, blend);
            if (!gMetal3D.linePipelines[blend]) {
                return false;
            }
        }
    }
    return true;
}

static NSUInteger pscalMetalAlignUp(NSUInteger value, NSUInteger alignment) {
    if (alignment == 0) {
        return value;
    }
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static NSUInteger pscalMetalNextPow2(NSUInteger value) {
    if (value <= 1u) {
        return 1u;
    }
    value--;
    for (NSUInteger shift = 1u; shift < sizeof(NSUInteger) * 8u; shift <<= 1u) {
        value |= value >> shift;
    }
    return value + 1u;
}

static bool pscalMetalEnsureDynamicVertexBuffers(NSUInteger requiredBytes) {
    NSUInteger target = gMetal3D.dynamicVertexBufferSize;
    if (target == 0u) {
        target = kPscalMetalInitialDynamicVertexBytes;
    }
    if (requiredBytes > target) {
        target = pscalMetalNextPow2(requiredBytes);
    }
    if (target == gMetal3D.dynamicVertexBufferSize &&
        gMetal3D.dynamicVertexBuffers[0] &&
        gMetal3D.dynamicVertexBuffers[1] &&
        gMetal3D.dynamicVertexBuffers[2]) {
        return true;
    }

    id<MTLBuffer> buffers[3] = { nil, nil, nil };
    for (int i = 0; i < 3; ++i) {
        buffers[i] = [gMetal3D.device newBufferWithLength:target
                                                   options:MTLResourceStorageModeShared];
        if (!buffers[i]) {
            return false;
        }
    }
    for (int i = 0; i < 3; ++i) {
        gMetal3D.dynamicVertexBuffers[i] = buffers[i];
    }
    gMetal3D.dynamicVertexBufferSize = target;
    gMetal3D.dynamicVertexBufferOffset = 0u;
    return true;
}

bool pscalMetal3DIsSupported(void) {
    return true;
}

bool pscalMetal3DEnsureRenderer(struct SDL_Renderer* renderer) {
    if (!renderer) {
        return false;
    }

    if (gMetal3D.renderer == renderer && gMetal3D.layer && gMetal3D.device && gMetal3D.queue &&
        gMetal3D.library) {
        return true;
    }

    pscalMetal3DShutdown();

    CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_RenderGetMetalLayer((SDL_Renderer*)renderer);
    if (!layer) {
        return false;
    }

    id<MTLDevice> device = layer.device;
    if (!device) {
        device = MTLCreateSystemDefaultDevice();
        layer.device = device;
    }
    if (!device) {
        return false;
    }

    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue) {
        return false;
    }

    NSError* libraryError = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:pscalMetalShaderSource()
                                                   options:nil
                                                     error:&libraryError];
    if (!library) {
        return false;
    }

    if (layer.pixelFormat == MTLPixelFormatInvalid) {
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    }
    layer.framebufferOnly = YES;

    gMetal3D.renderer = renderer;
    gMetal3D.layer = layer;
    gMetal3D.device = device;
    gMetal3D.queue = queue;
    gMetal3D.library = library;
    gMetal3D.viewportX = 0;
    gMetal3D.viewportY = 0;
    gMetal3D.viewportWidth = 1;
    gMetal3D.viewportHeight = 1;
    gMetal3D.dynamicVertexBufferSize = 0u;
    gMetal3D.dynamicVertexBufferOffset = 0u;
    gMetal3D.dynamicVertexBufferIndex = -1;
    if (!pscalMetalEnsurePipelines()) {
        return false;
    }
    return pscalMetalEnsureDynamicVertexBuffers(kPscalMetalInitialDynamicVertexBytes);
}

void pscalMetal3DSetViewport(int x, int y, int width, int height) {
    gMetal3D.viewportX = x;
    gMetal3D.viewportY = y;
    gMetal3D.viewportWidth = width > 0 ? width : 1;
    gMetal3D.viewportHeight = height > 0 ? height : 1;
}

bool pscalMetal3DBeginFrame(bool clearColor, const float clearColorRgba[4],
                            bool clearDepth, float clearDepthValue) {
    if (!gMetal3D.layer || !gMetal3D.device || !gMetal3D.queue) {
        return false;
    }
    if (gMetal3D.viewportWidth <= 0 || gMetal3D.viewportHeight <= 0) {
        return false;
    }

    if (gMetal3D.frameActive) {
        return true;
    }
    if (!pscalMetalEnsureDynamicVertexBuffers(kPscalMetalInitialDynamicVertexBytes)) {
        return false;
    }

    gMetal3D.layer.drawableSize = CGSizeMake((CGFloat)gMetal3D.viewportWidth,
                                             (CGFloat)gMetal3D.viewportHeight);
    if (!pscalMetalEnsureDepthTexture()) {
        return false;
    }

    id<CAMetalDrawable> drawable = [gMetal3D.layer nextDrawable];
    if (!drawable) {
        return false;
    }
    id<MTLCommandBuffer> commandBuffer = [gMetal3D.queue commandBuffer];
    if (!commandBuffer) {
        return false;
    }

    MTLRenderPassDescriptor* pass = [[MTLRenderPassDescriptor alloc] init];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].loadAction = clearColor ? MTLLoadActionClear : MTLLoadActionLoad;
    if (clearColor && clearColorRgba) {
        pass.colorAttachments[0].clearColor =
            MTLClearColorMake(clearColorRgba[0], clearColorRgba[1], clearColorRgba[2], clearColorRgba[3]);
    }

    pass.depthAttachment.texture = gMetal3D.depthTexture;
    pass.depthAttachment.storeAction = MTLStoreActionDontCare;
    pass.depthAttachment.loadAction = clearDepth ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.depthAttachment.clearDepth = clearDepthValue;

    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) {
        return false;
    }

    MTLViewport viewport = {
        .originX = 0.0,
        .originY = 0.0,
        .width = (double)gMetal3D.viewportWidth,
        .height = (double)gMetal3D.viewportHeight,
        .znear = 0.0,
        .zfar = 1.0
    };
    [encoder setViewport:viewport];
    [encoder setCullMode:MTLCullModeNone];

    gMetal3D.dynamicVertexBufferIndex = (gMetal3D.dynamicVertexBufferIndex + 1) % 3;
    gMetal3D.dynamicVertexBufferOffset = 0u;

    gMetal3D.drawable = drawable;
    gMetal3D.commandBuffer = commandBuffer;
    gMetal3D.encoder = encoder;
    gMetal3D.frameActive = true;
    return true;
}

static bool pscalMetalDraw(MTLPrimitiveType primitiveType,
                           const PscalMetalVertex* vertices,
                           size_t vertexCount,
                           bool depthTestEnabled,
                           bool depthWriteEnabled,
                           unsigned int depthFunc,
                           bool blendEnabled,
                           unsigned int blendSrc,
                           unsigned int blendDst) {
    if (!vertices || vertexCount == 0) {
        return true;
    }
    if (!gMetal3D.frameActive && !pscalMetal3DBeginFrame(true, NULL, true, 1.0f)) {
        return false;
    }
    if (!gMetal3D.encoder) {
        return false;
    }
    int blendIndex = pscalMetalBlendIndex(blendEnabled, blendSrc, blendDst);
    id<MTLRenderPipelineState> pipeline =
        (primitiveType == MTLPrimitiveTypeLine)
            ? gMetal3D.linePipelines[blendIndex]
            : gMetal3D.triPipelines[blendIndex];
    if (!pipeline) {
        return false;
    }

    NSUInteger byteLength = (NSUInteger)(vertexCount * sizeof(PscalMetalVertex));
    NSUInteger alignedOffset = pscalMetalAlignUp(gMetal3D.dynamicVertexBufferOffset, 256u);
    NSUInteger required = alignedOffset + byteLength;
    if (!pscalMetalEnsureDynamicVertexBuffers(required)) {
        return false;
    }
    if (required > gMetal3D.dynamicVertexBufferSize) {
        return false;
    }
    id<MTLBuffer> vertexBuffer = gMetal3D.dynamicVertexBuffers[gMetal3D.dynamicVertexBufferIndex];
    if (!vertexBuffer || ![vertexBuffer contents]) {
        return false;
    }
    memcpy(((uint8_t*)[vertexBuffer contents]) + alignedOffset, vertices, (size_t)byteLength);
    gMetal3D.dynamicVertexBufferOffset = required;

    [gMetal3D.encoder setRenderPipelineState:pipeline];
    [gMetal3D.encoder setDepthStencilState:pscalMetalDepthState(depthTestEnabled, depthWriteEnabled, depthFunc)];
    [gMetal3D.encoder setVertexBuffer:vertexBuffer offset:alignedOffset atIndex:0];
    [gMetal3D.encoder drawPrimitives:primitiveType vertexStart:0 vertexCount:(NSUInteger)vertexCount];
    return true;
}

bool pscalMetal3DDrawTriangles(const PscalMetalVertex* vertices, size_t vertexCount,
                               bool depthTestEnabled, bool depthWriteEnabled, unsigned int depthFunc,
                               bool blendEnabled, unsigned int blendSrc, unsigned int blendDst) {
    return pscalMetalDraw(MTLPrimitiveTypeTriangle,
                          vertices, vertexCount,
                          depthTestEnabled, depthWriteEnabled, depthFunc,
                          blendEnabled, blendSrc, blendDst);
}

bool pscalMetal3DDrawLines(const PscalMetalVertex* vertices, size_t vertexCount,
                           bool depthTestEnabled, bool depthWriteEnabled, unsigned int depthFunc,
                           bool blendEnabled, unsigned int blendSrc, unsigned int blendDst) {
    return pscalMetalDraw(MTLPrimitiveTypeLine,
                          vertices, vertexCount,
                          depthTestEnabled, depthWriteEnabled, depthFunc,
                          blendEnabled, blendSrc, blendDst);
}

void pscalMetal3DPresent(void) {
    if (!gMetal3D.frameActive) {
        return;
    }
    if (gMetal3D.encoder) {
        [gMetal3D.encoder endEncoding];
    }
    if (gMetal3D.commandBuffer && gMetal3D.drawable) {
        [gMetal3D.commandBuffer presentDrawable:gMetal3D.drawable];
        [gMetal3D.commandBuffer commit];
    }
    gMetal3D.drawable = nil;
    gMetal3D.encoder = nil;
    gMetal3D.commandBuffer = nil;
    gMetal3D.frameActive = false;
}

void pscalMetal3DShutdown(void) {
    if (gMetal3D.encoder) {
        [gMetal3D.encoder endEncoding];
    }
    gMetal3D.frameActive = false;
    gMetal3D.renderer = NULL;
    gMetal3D.layer = nil;
    gMetal3D.device = nil;
    gMetal3D.queue = nil;
    gMetal3D.library = nil;
    gMetal3D.depthTexture = nil;
    gMetal3D.drawable = nil;
    gMetal3D.commandBuffer = nil;
    gMetal3D.encoder = nil;
    gMetal3D.dynamicVertexBufferSize = 0u;
    gMetal3D.dynamicVertexBufferOffset = 0u;
    gMetal3D.dynamicVertexBufferIndex = -1;
    for (int blend = 0; blend < 3; ++blend) {
        gMetal3D.triPipelines[blend] = nil;
        gMetal3D.linePipelines[blend] = nil;
    }
    for (int i = 0; i < 3; ++i) {
        gMetal3D.dynamicVertexBuffers[i] = nil;
    }
    for (int writeIndex = 0; writeIndex < 2; ++writeIndex) {
        for (int funcIndex = 0; funcIndex < 8; ++funcIndex) {
            gMetal3D.depthStates[writeIndex][funcIndex] = nil;
        }
    }
}

#else

bool pscalMetal3DIsSupported(void) { return false; }
bool pscalMetal3DEnsureRenderer(struct SDL_Renderer* renderer) {
    (void)renderer;
    return false;
}
void pscalMetal3DSetViewport(int x, int y, int width, int height) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}
bool pscalMetal3DBeginFrame(bool clearColor, const float clearColorRgba[4],
                            bool clearDepth, float clearDepthValue) {
    (void)clearColor;
    (void)clearColorRgba;
    (void)clearDepth;
    (void)clearDepthValue;
    return false;
}
bool pscalMetal3DDrawTriangles(const PscalMetalVertex* vertices, size_t vertexCount,
                               bool depthTestEnabled, bool depthWriteEnabled, unsigned int depthFunc,
                               bool blendEnabled, unsigned int blendSrc, unsigned int blendDst) {
    (void)vertices;
    (void)vertexCount;
    (void)depthTestEnabled;
    (void)depthWriteEnabled;
    (void)depthFunc;
    (void)blendEnabled;
    (void)blendSrc;
    (void)blendDst;
    return false;
}
bool pscalMetal3DDrawLines(const PscalMetalVertex* vertices, size_t vertexCount,
                           bool depthTestEnabled, bool depthWriteEnabled, unsigned int depthFunc,
                           bool blendEnabled, unsigned int blendSrc, unsigned int blendDst) {
    (void)vertices;
    (void)vertexCount;
    (void)depthTestEnabled;
    (void)depthWriteEnabled;
    (void)depthFunc;
    (void)blendEnabled;
    (void)blendSrc;
    (void)blendDst;
    return false;
}
void pscalMetal3DPresent(void) {}
void pscalMetal3DShutdown(void) {}

#endif
