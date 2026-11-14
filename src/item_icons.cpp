#include "item_icons.hpp"
#include <SOIL2/SOIL2.h>
#include <unordered_map>
#include <unordered_set>

namespace ItemIcons {
	static GLuint makeTexture(std::string_view png) {
		GLuint texture = SOIL_load_OGL_texture_from_memory(
			(const unsigned char*)png.data(), (int)png.size(),
			SOIL_LOAD_AUTO, SOIL_CREATE_NEW_ID, SOIL_FLAG_MIPMAPS);
		if (texture == 0) return 0;

		glBindTexture(GL_TEXTURE_2D, texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glBindTexture(GL_TEXTURE_2D, 0);
		return texture;
	}

	std::function<GLuint(std::string_view)> make = makeTexture;
	std::function<void(GLuint)> release = [](GLuint texture) { glDeleteTextures(1, &texture); };

	static uint64_t madeFor = 0;
	// 0 is kept too, so a type with no icon or a broken one isn't tried every frame
	static std::unordered_map<int, GLuint> textures;
	static std::unordered_set<GLuint> kept;

	void keepFor(const ItemCatalog::Catalog* catalog) {
		uint64_t version = catalog ? catalog->version : 0;
		if (version == madeFor) return;
		clear();
		madeFor = version;
	}

	GLuint get(const ItemCatalog::Catalog* catalog, int type) {
		if (!catalog) return 0;
		keepFor(catalog);

		auto found = textures.find(type);
		if (found != textures.end()) return found->second;

		std::string_view png = catalog->icon(type);
		GLuint texture = png.empty() ? 0 : make(png);
		textures[type] = texture;
		if (texture != 0) kept.insert(texture);
		return texture;
	}

	bool owns(GLuint texture) {
		return texture != 0 && kept.count(texture) > 0;
	}

	size_t count() {
		size_t made = 0;
		for (const auto& [type, texture] : textures) if (texture != 0) made++;
		return made;
	}

	void clear() {
		for (const auto& [type, texture] : textures) if (texture != 0) release(texture);
		textures.clear();
		kept.clear();
	}
}
