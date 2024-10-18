struct CommonUniforms {
    projMat: mat4x4<f32>,
    viewMat: mat4x4<f32>,
    modelMat: mat4x4<f32>,
    params1: vec4<f32>, // viewportScale.xy, lineWidth
    params2: vec4<f32>, // alphaTestF
    lightPos: vec4<f32>,
    lightColor: vec4<f32>,
};

@group(0) @binding(0) var<uniform> commonUniforms: CommonUniforms;

struct VertexInput {
    @location(0) aPosition: vec3<f32>,
    @location(1) aNext: vec3<f32>,
    @location(2) aNormal: vec3<f32>,
    @location(3) aColor0: vec4<f32>,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) vColor0: vec4<f32>,
};

struct FragmentOutput {
    @location(0) Color: vec4<f32>,
};

@vertex
fn mainVert(input: VertexInput) -> VertexOutput {
    var mvpMat: mat4x4<f32> = commonUniforms.projMat * commonUniforms.viewMat;// * commonUniforms.modelMat;
    var mvMat: mat4x4<f32> = commonUniforms.viewMat;// * commonUniforms.modelMat;

    var projStartPos: vec4<f32> = mvpMat * vec4<f32>(input.aPosition, 1.0);
    var projEndPos: vec4<f32> = mvpMat * vec4<f32>(input.aNext, 1.0);

    projStartPos.x /= projStartPos.w;
    projStartPos.y /= projStartPos.w;
    projStartPos.z /= projStartPos.w;
    projEndPos.x /= projEndPos.w;
    projEndPos.y /= projEndPos.w;
    projEndPos.z /= projEndPos.w;

    let dp = projEndPos - projStartPos;
    var delta = normalize(dp.xy);
    delta = vec2<f32>(-delta.y, delta.x);
    var realDelta = vec2<f32>(0.0, 0.0);

    realDelta += delta * input.aNormal.x;
    realDelta = realDelta * commonUniforms.params1.z;

    var output: VertexOutput;
    var clipSpace = mvpMat * vec4<f32>(input.aPosition, 1.0);
    clipSpace.x /= clipSpace.w;
    clipSpace.y /= clipSpace.w;
    clipSpace.z /= clipSpace.w;
    clipSpace += vec4<f32>(realDelta.xy * commonUniforms.params1.xy, 0.0, 0.0);
    clipSpace.z = 1.0;

    if (projStartPos.w < 0.01) {
        clipSpace.w = -1.0;
    } else {
        clipSpace.w = 1.0;
    }
    output.position = clipSpace;
    output.vColor0 = input.aColor0;

    return output;
}

@fragment
fn mainFrag(input: VertexOutput) -> FragmentOutput {
    var out: FragmentOutput;
    out.Color = input.vColor0;
    return out;
}
