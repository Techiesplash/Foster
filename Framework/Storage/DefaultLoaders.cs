using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace Foster.Framework.Storage
{
	public static class DefaultLoaders
	{
		public static Dictionary<Type, IAssetLoader> Dictionary = new()
		{
			{ typeof(string), new TextLoader() },
			{ typeof(Texture), new TextureLoader() },
			{ typeof(Image), new ImageLoader() },
			{ typeof(Font), new FontLoader() },
			{ typeof(SpriteFont), new SpriteFontLoader() },
			{ typeof(Aseprite), new AsepriteLoader() }
		};

		public class TextLoader : AssetLoader<string>
		{
			public override string Load(Content content, string relativePath, params object[] args)
			{
				return content.ReadAllText(relativePath);
			}
		}

		public class TextureLoader : AssetLoader<Texture>
		{
			public override Texture Load(Content content, string relativePath, params object[] args)
			{
				return new Texture(content.Load<Image>(relativePath));
			}
		}

		public class ImageLoader : AssetLoader<Image>
		{
			public override Image Load(Content content, string relativePath, params object[] args)
			{
				using var stream = content.OpenRead(relativePath);
				return new Image(stream);
			}
		}

		public class FontLoader : AssetLoader<Font>
		{
			public override Font Load(Content content, string relativePath, params object[] args)
			{
				using var stream = content.OpenRead(relativePath);
				return new Font(stream);
			}
		}

		public class SpriteFontLoader : AssetLoader<SpriteFont>
		{
			public float DefaultSize = 16f;

			public override SpriteFont Load(Content content, string relativePath, params object[] args)
			{
				using var font = content.Load<Font>(relativePath);

				// arg1: size (float or int)
				if (args.Length > 0)
				{
					var size = args[0] switch
					{
						float f => f,
						int i => i,
						_ => DefaultSize
					};
					return new SpriteFont(font, size);
				}
				else
				{
					return new SpriteFont(font, DefaultSize);
				}
			}
		}

		public class AsepriteLoader : AssetLoader<Aseprite>
		{
			public override Aseprite Load(Content content, string relativePath, params object[] args)
			{
				using var stream = content.OpenRead(relativePath);
				return new Aseprite(stream);
			}
		}
	}
}
