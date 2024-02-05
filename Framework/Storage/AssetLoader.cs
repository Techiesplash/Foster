using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace Foster.Framework.Storage
{
	/// <summary>
	/// Can be associated with a Content to perform loading logic for a type
	/// </summary>
	public interface IAssetLoader
	{
		public object Load(Content content, string relativePath, params object[] args);
		public Type AssetType { get; }
	}

	public abstract class AssetLoader<T> : IAssetLoader
	{
		public Type AssetType => typeof(T);

		public virtual T Load(Content content, string relativePath, params object[] args)
		{
			throw new NotImplementedException();
		}

		object IAssetLoader.Load(Content content, string relativePath, params object[] args)
			=> Load(content, relativePath, args);
	}
}
