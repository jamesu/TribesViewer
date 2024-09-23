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
    @location(1) aNormal: vec3<f32>,
    @location(2) aColor0: vec4<f32>,
    @location(3) aNext: vec3<f32>,
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
    let mvMat: mat4x4<f32> = commonUniforms.viewMat * commonUniforms.modelMat;

    let startPos = mvMat * vec4<f32>(input.aPosition, 1.0);
    let endPos = mvMat * vec4<f32>(input.aNext, 1.0);
    let projStartPos = commonUniforms.projMat * startPos;
    let projEndPos = commonUniforms.projMat * endPos;
    let dp = projEndPos - projStartPos;
    var delta = normalize(vec4<f32>(dp.x, dp.y, 0.0, 0.0));
    delta = vec4<f32>(-delta.y, delta.x, 0.0, 0.0);
    var realDelta = vec4<f32>(0.0, 0.0, 0.0, 0.0);
    realDelta += delta * input.aNormal.x;
    realDelta = realDelta * commonUniforms.params1.z;

    let mvpMat: mat4x4<f32> = commonUniforms.projMat * commonUniforms.viewMat * commonUniforms.modelMat;

    var output: VertexOutput;
    var gl_Position = mvpMat * vec4<f32>(input.aPosition, 1.0);
    gl_Position.x /= gl_Position.w;
    gl_Position.y /= gl_Position.w;
    gl_Position.z /= gl_Position.w;
    gl_Position += vec4<f32>(realDelta.xy * commonUniforms.params1.xy, 0.0, 0.0);
    gl_Position.z = 1.0;
    gl_Position.w = 1.0;

    output.position = gl_Position;
    output.vColor0 = input.aColor0;

    return output;
}

@fragment
fn mainFrag(input: VertexOutput) -> FragmentOutput {
    var out: FragmentOutput;
    out.Color = input.vColor0;
    return out;
}
