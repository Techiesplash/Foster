using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Linq.Expressions;
using System.Text;
using System.Threading.Tasks;

namespace Foster.Framework.Storage
{
	/// <summary>
	/// Default Content implementation.
	/// Should work well for PC, etc, but can be overridden for custom handling.
	/// </summary>
	public class Content
	{
		public string CurrentDirectory { get; set; } = "";

		protected Dictionary<Type, IAssetLoader> _assetLoaders = DefaultLoaders.Dictionary;
		public IReadOnlyDictionary<Type, IAssetLoader> AssetLoaders => _assetLoaders.AsReadOnly();

		private class ContentEnumerator : IEnumerator<string>
		{
			public string[] Locations;
			public int Index = -1;

			public string Current
			{
				get
				{
					try
					{
						return Locations[Index];
					}
					catch (IndexOutOfRangeException)
					{
						throw new InvalidOperationException();
					}
				}
			}

			object IEnumerator.Current => Current;

			public ContentEnumerator(string[] locations)
			{
				Locations = locations;
			}

			public bool MoveNext()
			{
				Index++;
				if (Index >= Locations.Length)
					return false;
				return true;
			}

			public void Reset() => Index = -1;

			public void Dispose() { }
		}

		public Content() { }
		public Content(string content) : this()
		{
			CurrentDirectory = content;
		}

		#region Directory
		public virtual bool FileExists(string relativePath)
		{
			return File.Exists(Path.Join(CurrentDirectory, relativePath));
		}
		public virtual bool DirectoryExists(string relativePath)
		{
			return Directory.Exists(Path.Join(CurrentDirectory, relativePath));
		}
		public virtual bool Exists(string name)
		{
			return FileExists(name) || DirectoryExists(name);
		}

		public virtual IEnumerator<string> EnumerateFiles(string path, string searchPattern, bool recursive)
		{
			return new ContentEnumerator(
					Directory.GetFiles(
						CurrentDirectory + path,
						searchPattern,
						recursive ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly
						));
		}

		public virtual IEnumerator<string> EnumerateDirectories(string path, string searchPattern, bool recursive)
		{

			return new ContentEnumerator(
				Directory.GetDirectories(
					CurrentDirectory + path,
					searchPattern,
					recursive ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly
					));
		}

		#endregion

		#region File

		public virtual Stream OpenRead(string relativePath)
		{
			return File.OpenRead(Path.Join(CurrentDirectory, relativePath));
		}

		public virtual byte[] ReadAllBytes(string relativePath)
		{
			return File.ReadAllBytes(Path.Join(CurrentDirectory, relativePath));
		}

		public virtual string ReadAllText(string relativePath)
		{
			return File.ReadAllText(Path.Join(CurrentDirectory, relativePath));
		}

		#endregion

		#region Asset Loading

		public virtual T Load<T>(string relativePath, params object[] args)
		{
			if (_assetLoaders.TryGetValue(typeof(T), out var loader))
			{
				return (T)loader.Load(this, relativePath, args);
			}

			throw new InvalidOperationException($"No loader found for type {typeof(T)}");
		}

		public virtual void RegisterLoader(IAssetLoader loader)
		{
			_assetLoaders[loader.AssetType] = loader;
		}

		public virtual void UnregisterLoader(IAssetLoader loader)
		{
			_assetLoaders.Remove(loader.AssetType);
		}

		#endregion
	}
}
