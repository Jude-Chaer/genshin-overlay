#pragma once
#include <GL/glew.h>
#include <cstddef>
#include <functional>
#include <string_view>
#include "item_catalog.hpp"

// One GL texture per item type, made from the catalog's icon bytes the first
// time it's asked for. A new catalog version frees them all. Render thread only.

namespace ItemIcons {
	// frees what was made for any other catalog version
	void keepFor(const ItemCatalog::Catalog* catalog);
	// the type's texture, 0 when it has no icon or there's no catalog
	GLuint get(const ItemCatalog::Catalog* catalog, int type);
	// one of the textures kept right now
	bool owns(GLuint texture);
	// textures made and kept right now
	size_t count();
	// frees every texture it keeps
	void clear();

	// how a texture is made from png bytes and freed (the tests run with no GL)
	extern std::function<GLuint(std::string_view png)> make;
	extern std::function<void(GLuint texture)> release;
}
