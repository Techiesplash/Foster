namespace Foster.Framework;

internal static class ShaderDefaults
{
	public static Dictionary<Renderers, ShaderCreateInfo> Batcher = new()
	{
		[Renderers.OpenGL] = new()
		{
			VertexShader =
				@"#version 330
				uniform mat4 u_matrix;
				layout(location=0) in vec2 a_position;
				layout(location=1) in vec2 a_tex;
				layout(location=2) in vec4 a_color;
				layout(location=3) in vec4 a_type;
				out vec2 v_tex;
				out vec4 v_col;
				out vec4 v_type;
				void main(void)
				{
					gl_Position = u_matrix * vec4(a_position.xy, 0, 1);
					v_tex = a_tex;
					v_col = a_color;
					v_type = a_type;
				}",
			FragmentShader =
				@"#version 330
				uniform sampler2D u_texture;
				in vec2 v_tex;
				in vec4 v_col;
				in vec4 v_type;
				out vec4 o_color;
				void main(void)
				{
					vec4 color = texture(u_texture, v_tex);
					o_color = 
						v_type.x * color * v_col + 
						v_type.y * color.a * v_col + 
						v_type.z * v_col;
				}"
		},
		[Renderers.D3D11] = new()
		{
			VertexShader =
				@"cbuffer Constants : register(b0)
				{
					float4x4 u_matrix;
				};
				struct VertexInput
				{
					float2 position : POSITION;
					float2 tex : TEXCOORD0;
					uint color : COLOR0;
					uint type : COLOR1;
				};
				struct PixelInput
				{
					float4 position : SV_POSITION;
					float2 tex : TEXCOORD0;
					float4 color : COLOR0;
					float4 type : COLOR1;
				};
				PixelInput vt_main(VertexInput input)
				{
					PixelInput output;
					output.position = mul(u_matrix, float4(input.position.xy, 0, 1));
					output.tex = input.tex;
					output.color = float4((input.color & 0xFF), (input.color & 0xFF00) >> 8, (input.color & 0xFF0000) >> 16, (input.color & 0xFF000000) >> 24) / 255.0;
			    	output.type = float4((input.type & 0xFF), ((input.type >> 8) & 0xFF), ((input.type >> 16) & 0xFF), ((input.type >> 24) & 0xFF));
					return output;
				}",
			FragmentShader =
				@"struct PixelInput
				{
					float4 position : SV_POSITION;
					float2 tex : TEXCOORD0;
					float4 color : COLOR0;
					float4 type : COLOR1;
				};
				Texture2D u_texture : register(t0);
				SamplerState u_texture_sampler : register(s0);
				float4 px_main(PixelInput input) : SV_TARGET
				{
					float4 colT = u_texture.Sample(u_texture_sampler, input.tex);
					return float4(input.color.rgb, colT.a);
				}"
		}
	};
}
