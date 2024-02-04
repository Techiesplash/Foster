namespace Foster.Framework;

internal static class ShaderDefaults
{
	public static Dictionary<Renderers, ShaderCreateInfo> Batcher = new()
	{
		[Renderers.OpenGL] = new()
		{
			VertexShader =
				@"#version 330\n
				uniform mat4 u_matrix;\n
				layout(location=0) in vec2 a_position;\n
				layout(location=1) in vec2 a_tex;\n
				layout(location=2) in vec4 a_color;\n
				layout(location=3) in vec4 a_type;\n
				out vec2 v_tex;\n
				out vec4 v_col;\n
				out vec4 v_type;\n
				void main(void)\n
				{\n
					gl_Position = u_matrix * vec4(a_position.xy, 0, 1);\n
					v_tex = a_tex;\n
					v_col = a_color;\n
					v_type = a_type;\n
				}",
			FragmentShader =
				@"#version 330\n
				uniform sampler2D u_texture;\n
				in vec2 v_tex;\n
				in vec4 v_col;\n
				in vec4 v_type;\n
				out vec4 o_color;\n
				void main(void)\n
				{\n
					vec4 color = texture(u_texture, v_tex);\n
					o_color = \n
						v_type.x * color * v_col + \n
						v_type.y * color.a * v_col + \n
						v_type.z * v_col;\n
				}"
		},
		[Renderers.D3D11] = new()
		{
			VertexShader =
				@"cbuffer Constants : register(b0)\n
				{\n
					float4x4 u_matrix;\n
				};\n
				struct VertexInput\n
				{\n
					float2 position : POSITION;\n
					float2 tex : TEXCOORD0;\n
					uint color : COLOR0;\n
					uint type : COLOR1;\n
				};\n
				struct PixelInput\n
				{\n
					float4 position : SV_POSITION;\n
					float2 tex : TEXCOORD0;\n
					float4 color : COLOR0;\n
					float4 type : COLOR1;\n
				};\n
				PixelInput main(VertexInput input)\n
				{\n
					PixelInput output;\n
					output.position = mul(u_matrix, float4(input.position.xy, 0, 1));\n
					output.tex = input.tex;\n
					output.color = float4((input.color & 0xFF), ((input.color) & 0xFF00) >> 8, ((input.color) & 0xFF0000) >> 16, ((input.color) & 0xFF000000) >> 24) / 255.0;\n
			    	output.type = float4((input.type & 0xFF), ((input.type >> 8) & 0xFF), ((input.type >> 16) & 0xFF), ((input.type >> 24) & 0xFF));\n
					return output;\n
				}",
			FragmentShader =
				@"struct PixelInput
				{
					float4 position : SV_POSITION;
					float2 tex : TEXCOORD0;
					float4 color : COLOR0;
                    // R = Multiply, G = Wash, B = Fill, A = Padding
					float4 type : COLOR1;
				};
				Texture2D u_texture : register(t0);
				SamplerState u_texture_sampler : register(s0);
				float4 main(PixelInput input) : SV_TARGET
				{
					float4 colT = u_texture.Sample(u_texture_sampler, input.tex);
					return float4(input.color.rgb, colT.a);
				}"
		}
	};
}
