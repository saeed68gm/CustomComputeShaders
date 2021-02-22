#include "ComputeShaderDeclaration.h"

#include "Modules/ModuleManager.h"

#define NUM_THREADS_PER_GROUP_DIMENSION 32

/// <summary>
/// Internal class thet holds the parameters and connects the HLSL Shader to the engine
/// </summary>
class FWhiteNoiseCS : public FGlobalShader
{
public:
	//Declare this class as a global shader
	DECLARE_GLOBAL_SHADER(FWhiteNoiseCS);
	//Tells the engine that this shader uses a structure for its parameters
	SHADER_USE_PARAMETER_STRUCT(FWhiteNoiseCS, FGlobalShader);
	/// <summary>
	/// DECLARATION OF THE PARAMETER STRUCTURE
	/// The parameters must match the parameters in the HLSL code
	/// For each parameter, provide the C++ type, and the name (Same name used in HLSL code)
	/// </summary>
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

public:
	//Called by the engine to determine which permutations to compile for this shader
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	//Modifies the compilations environment of the shader
	static inline void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		//We're using it here to add some preprocessor defines. That way we don't have to change both C++ and HLSL code when we change the value for NUM_THREADS_PER_GROUP_DIMENSION
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), NUM_THREADS_PER_GROUP_DIMENSION);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), NUM_THREADS_PER_GROUP_DIMENSION);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Z"), 1);
	}

};

// This will tell the engine to create the shader and where the shader entry point is.
//                        ShaderType              ShaderPath             Shader function name    Type
IMPLEMENT_GLOBAL_SHADER(FWhiteNoiseCS, "/CustomShaders/WhiteNoiseCS.usf", "MainComputeShader", SF_Compute);


//Static members
FWhiteNoiseCSManager* FWhiteNoiseCSManager::instance = nullptr;

//Begin the execution of the compute shader each frame
void FWhiteNoiseCSManager::BeginRendering()
{
	ENQUEUE_RENDER_COMMAND(CaptureCommand)
		(
			[](FRHICommandListImmediate& RHICmdList)
			{
				FWhiteNoiseCSManager::Get()->UpdateResults(RHICmdList);
			}
		);
	
}

//Stop the compute shader execution
void FWhiteNoiseCSManager::EndRendering()
{
}

//Update the parameters by a providing an instance of the Parameters structure used by the shader manager
void FWhiteNoiseCSManager::UpdateParameters(FWhiteNoiseCSParameters& params)
{
	cachedParams = params;
	bCachedParamsAreValid = true;
}

void FWhiteNoiseCSManager::UpdateResults(FRHICommandListImmediate& RHICmdList)
{
	if (!(bCachedParamsAreValid && cachedParams.RenderTarget))
	{
		return;
	}

	//Render Thread Assertion
	check(IsInRenderingThread());
	const ERHIFeatureLevel::Type FeatureLevel = GMaxRHIFeatureLevel;
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(FeatureLevel);
    FTexture2DRHIRef OutTexture = cachedParams.RenderTarget->GetRenderTargetResource()->GetRenderTargetTexture();
	FPooledRenderTargetDesc TexDesc = FPooledRenderTargetDesc::Create2DDesc(cachedParams.GetRenderTargetSize(),
															cachedParams.RenderTarget->GetRenderTargetResource()->TextureRHI->GetFormat(),
															FClearValueBinding::None, TexCreate_None,
															TexCreate_ShaderResource | TexCreate_UAV, false);
	FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
											cachedParams.GetRenderTargetSize(),
											PF_R32_UINT,
											FClearValueBinding::None,
											/* InFlags = */ TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureDesc DescFloat = FRDGTextureDesc::Create2D(
											cachedParams.GetRenderTargetSize(),
											PF_R16F,
											FClearValueBinding::None,
											/* InFlags = */ TexCreate_ShaderResource | TexCreate_UAV);
    // FRDGTextureDesc TexDesc = FRDGTextureDesc::Create2DDesc(FluidSurfaceSize, PF_G32R32F, FClearValueBinding(FLinearColor::Black), TexCreate_None, TexCreate_UAV | TexCreate_ShaderResource, false);
    TRefCountPtr<IPooledRenderTarget> PooledCustomTexture;
    GRenderTargetPool.FindFreeElement(RHICmdList, TexDesc, PooledCustomTexture, TEXT("CustomTexture"));
    FRDGBuilder GraphBuilder(RHICmdList);
	FRDGTextureRef CustomTexture = GraphBuilder.RegisterExternalTexture(PooledCustomTexture, TEXT("CustomTexture"),
																		  ERenderTargetTexture::ShaderResource, ERDGTextureFlags::MultiFrame);
	FRDGTextureUAVRef CustomTextureUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(CustomTexture));

    TShaderMapRef<FWhiteNoiseCS> WhiteNoiseCS(ShaderMap);
    FWhiteNoiseCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FWhiteNoiseCS::FParameters>();

	PassParameters->OutputTexture = CustomTextureUAV;

	FIntVector ThreadGroupCount(FMath::DivideAndRoundUp(cachedParams.GetRenderTargetSize().X, NUM_THREADS_PER_GROUP_DIMENSION),
								FMath::DivideAndRoundUp(cachedParams.GetRenderTargetSize().Y, NUM_THREADS_PER_GROUP_DIMENSION), 1);

	GraphBuilder.AddPass(RDG_EVENT_NAME("ComputeWhiteNoise"),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, WhiteNoiseCS, ThreadGroupCount](FRHICommandList &RHICmdList) {
				FComputeShaderUtils::Dispatch(RHICmdList, WhiteNoiseCS, *PassParameters, ThreadGroupCount);
			});

	// GraphBuilder.QueueTextureExtraction(CustomTexture, &PooledCustomTexture);
    GraphBuilder.Execute();
	RHICmdList.CopyTexture(PooledCustomTexture->GetRenderTargetItem().ShaderResourceTexture,  OutTexture->GetTexture2D(), FRHICopyTextureInfo());
}


/// <summary>
/// Creates an instance of the shader type parameters structure and fills it using the cached shader manager parameter structure
/// Gets a reference to the shader type from the global shaders map
/// Dispatches the shader using the parameter structure instance
/// </summary>
// void FWhiteNoiseCSManager::Execute_RenderThread(FRHICommandListImmediate& RHICmdList, class FSceneRenderTargets& SceneContext)
// {
// 	//If there's no cached parameters to use, skip
// 	//If no Render Target is supplied in the cachedParams, skip
// 	if (!(bCachedParamsAreValid && cachedParams.RenderTarget))
// 	{
// 		return;
// 	}

// 	//Render Thread Assertion
// 	check(IsInRenderingThread());


// 	//If the render target is not valid, get an element from the render target pool by supplying a Descriptor
// 	if (!ComputeShaderOutput.IsValid())
// 	{
// 		UE_LOG(LogTemp, Warning, TEXT("Not Valid"));
// 		FPooledRenderTargetDesc ComputeShaderOutputDesc(FPooledRenderTargetDesc::Create2DDesc(cachedParams.GetRenderTargetSize(), cachedParams.RenderTarget->GetRenderTargetResource()->TextureRHI->GetFormat(), FClearValueBinding::None, TexCreate_None, TexCreate_ShaderResource | TexCreate_UAV, false));
// 		ComputeShaderOutputDesc.DebugName = TEXT("WhiteNoiseCS_Output_RenderTarget");
// 		GRenderTargetPool.FindFreeElement(RHICmdList, ComputeShaderOutputDesc, ComputeShaderOutput, TEXT("WhiteNoiseCS_Output_RenderTarget"));
// 	}
	
// 	//Unbind the previously bound render targets
// 	// UnbindRenderTargets(RHICmdList);

// 	//Specify the resource transition, we're executing this in post scene rendering so we set it to Graphics to Compute
// 	RHICmdList.TransitionResource(EResourceTransitionAccess::ERWBarrier, EResourceTransitionPipeline::EGfxToCompute, ComputeShaderOutput->GetRenderTargetItem().UAV);


// 	//Fill the shader parameters structure with tha cached data supplied by the client
// 	FWhiteNoiseCS::FParameters PassParameters;
// 	PassParameters.OutputTexture = ComputeShaderOutput->GetRenderTargetItem().UAV;
// 	PassParameters.Dimensions = FVector2D(cachedParams.GetRenderTargetSize().X, cachedParams.GetRenderTargetSize().Y);
// 	PassParameters.TimeStamp = cachedParams.TimeStamp;

// 	//Get a reference to our shader type from global shader map
// 	TShaderMapRef<FWhiteNoiseCS> whiteNoiseCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

// 	//Dispatch the compute shader
// 	FComputeShaderUtils::Dispatch(RHICmdList, whiteNoiseCS, PassParameters,
// 		FIntVector(FMath::DivideAndRoundUp(cachedParams.GetRenderTargetSize().X, NUM_THREADS_PER_GROUP_DIMENSION),
// 			FMath::DivideAndRoundUp(cachedParams.GetRenderTargetSize().Y, NUM_THREADS_PER_GROUP_DIMENSION), 1));

// 	//Copy shader's output to the render target provided by the client
// 	RHICmdList.CopyTexture(ComputeShaderOutput->GetRenderTargetItem().ShaderResourceTexture, cachedParams.RenderTarget->GetRenderTargetResource()->TextureRHI, FRHICopyTextureInfo());

// }
