struct CommonUniforms {
    projMat: mat4x4<f32>,
    viewMat: mat4x4<f32>,
    modelMat: mat4x4<f32>,
    params1: vec4<f32>, // viewportScale.xy, lineWidth
    params2: vec4<f32>, // alphaTestF, squareSize, hmX, hmY
    lightPos: vec4<f32>,
    lightColor: vec4<f32>,

    sq01Tex : array<vec4<f32>, 16> // texcoord split over 2 vec4's
};

// Uniforms
@group(0) @binding(0) var<uniform> uniforms: CommonUniforms;

// Terrain textures
@group(1) @binding(0) var squareTextures: texture_2d_array<f32>; // Heightmap texture
@group(1) @binding(1) var gridMap: texture_2d<u32>;      // squareFlags(0...3(orient), 3...6(empty), 6(Grid45)), matIndex
@group(1) @binding(2) var heightMap: texture_2d<f32>;    // Heightmap texture

// Samplers
@group(1) @binding(3) var samplerPixel: sampler;
@group(1) @binding(4) var samplerLinear: sampler;


struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) texCoords: vec2<f32>,  // Pass texCoords to fragment
    @location(1) matIndex: u32,         // Pass material index to fragment
    @location(2) debugCol: vec4<f32>
};


@vertex
fn vertMain(@builtin(vertex_index) vertexID: u32) -> VertexOutput {
    var output: VertexOutput;

    // Each quad has 6 vertices (2 triangles), so get the quadID
    let quadID = vertexID / 6u;

    // Compute the grid position based on quadID
    let gridX = quadID % u32(uniforms.params2.z);
    let gridY = quadID / u32(uniforms.params2.z);

    // Compute texture coordinates for the terrainData texture to get quad parameters
    let texCoord = vec2<u32>(
        (gridX),  // X coordinate
        (gridY)   // Y coordinate
    );

    // Sample the terrain data for the current quad
    let quadData: vec4<u32> = textureLoad(gridMap, vec2<u32>(texCoord), 0);
    let matFlag = u32(quadData.r);
    let matIndex = u32(quadData.g);
    let texFlag = (u32(matFlag) & 0x7);
    let vertTexBase = texFlag * 2;
    
    // Extract flags
    let grid45 = (matFlag >> 6u) & (1u);
    let empty = (matFlag >> 3u) & (1u);

    // Check if the Empty flag is set
    if (empty == 1u) {
        // If the Empty flag is set, collapse the quad to a degenerate triangle by setting all vertices to the same point
        output.position = vec4<f32>(0.0, 0.0, 0.0, 1.0); // Degenerate position (all vertices collapsed)
        output.texCoords = vec2<f32>(0.0, 0.0); // No need to set meaningful texture coordinates
        output.matIndex = 0u; // Default material index
        return output;
    }

    // Define corner positions for each vertex (local quad space)
    let cornerPos = array<vec2<f32>, 4>(
        vec2<f32>(0.0, 1.0),  // Top-left
        vec2<f32>(1.0, 1.0),  // Top-right
        vec2<f32>(1.0, 0.0),  // Bottom-right
        vec2<f32>(0.0, 0.0)   // Bottom-left
    );

    var pos: vec2<f32>;
    var tex: vec2<f32>;

    // NOTE: top = bottom here. Left/right is ok.

    if (grid45 == 1u) {
        // Flip the triangle order when Grid45 flag is set
        switch (vertexID % 6u) {
            case 0u: { pos = cornerPos[0]; tex = uniforms.sq01Tex[(vertTexBase) + 0].xy; } // Top-left
            case 1u: { pos = cornerPos[1]; tex = uniforms.sq01Tex[(vertTexBase) + 0].zw; } // Top-right
            case 2u: { pos = cornerPos[3]; tex = uniforms.sq01Tex[(vertTexBase) + 1].zw; } // Bottom-left
            case 3u: { pos = cornerPos[3]; tex = uniforms.sq01Tex[(vertTexBase) + 1].zw; } // Bottom-left
            case 4u: { pos = cornerPos[1]; tex = uniforms.sq01Tex[(vertTexBase) + 0].zw; } // Top-right
            case 5u: { pos = cornerPos[2]; tex = uniforms.sq01Tex[(vertTexBase) + 1].xy; } // Bottom-right
            default: { pos = cornerPos[0]; tex = uniforms.sq01Tex[(vertTexBase) + 0].xy; }
        }
        //pos.x = 0.0;
        //pos.y = 0.0;
    } else {
        // Default triangle strip order
        switch (vertexID % 6u) {
            case 0u: { pos = cornerPos[0]; tex = uniforms.sq01Tex[(vertTexBase) + 0].xy; } // Top-left
            case 1u: { pos = cornerPos[1]; tex = uniforms.sq01Tex[(vertTexBase) + 0].zw; } // Top-right
            case 2u: { pos = cornerPos[2]; tex = uniforms.sq01Tex[(vertTexBase) + 1].xy; } // Bottom-right
            case 3u: { pos = cornerPos[2]; tex = uniforms.sq01Tex[(vertTexBase) + 1].xy; } // Bottom-right
            case 4u: { pos = cornerPos[3]; tex = uniforms.sq01Tex[(vertTexBase) + 1].zw; } // Bottom-left
            case 5u: { pos = cornerPos[0]; tex = uniforms.sq01Tex[(vertTexBase) + 0].xy; } // Top-left
            default: { pos = cornerPos[0]; tex = uniforms.sq01Tex[(vertTexBase) + 0].xy; }
        }
    }

    // Compute the quad's position in world space
    let squareSize = uniforms.params2.y;
    let quadPos = (vec2<f32>(f32(gridX), f32(gridY)) * squareSize) + (pos.xy * squareSize);

    // Sample the heightmap to get the Z value (height) for this vertex
    let heightTexCoord = vec2<u32>(
        (gridX + u32(pos.x)),
        (gridY + u32(pos.y))
    );
    let heightZ = textureLoad(heightMap, heightTexCoord, 0).r;

    // Local position in model space
    let localPosition = vec4<f32>(quadPos.xy, heightZ, 1.0);

    // Apply model, view, and projection matrices
    let worldPosition = uniforms.modelMat * localPosition;
    let viewPosition = uniforms.viewMat * worldPosition;
    let clipPosition = uniforms.projMat * viewPosition;

    // Output the clip space position and texture coordinates
    output.position = clipPosition;
    output.texCoords = tex.xy;
    output.matIndex = matIndex;
    //output.debugCol = vec4<f32>(f32((texFlag & 1u) == 1u), f32(((texFlag >> 1) & 1u) == 1u), f32(((texFlag >> 2) & 1u) == 1u), f32(texFlag == 7));
    output.debugCol = vec4<f32>(1.0, 0.0, 0.0, 0.0);

    return output;
}


@fragment
fn fragMain(input: VertexOutput) -> @location(0) vec4<f32> {
    // Sample the texture array at the given texCoords and matIndex
    let sampledColor = textureSample(squareTextures, samplerLinear, input.texCoords, input.matIndex);

    /*
    let debugSampledCol = (sampledColor.x + sampledColor.y + sampledColor.z) * (1.0/3.0);

    if (input.debugCol.a > 0.0 && input.texCoords.x > 0.95 && input.texCoords.y < 0.5)
    {
        return input.debugCol;
    }
    else if (input.debugCol.a > 0.0 && input.texCoords.y > 0.95)
    {
        return vec4<f32>(1.0,1.0,1.0,1.0);
    }
    else
    */
    {
        return sampledColor;
    }
}

