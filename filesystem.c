#include "filesystem.h"
#include "lutro.h"
#include "lutro_assert.h"

#include <compat/strl.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <vfs/vfs_implementation.h>
#include <string/stdstring.h>

// for getcwd...
#if defined(_WIN32)
#  include <direct.h>
#else
#  include <unistd.h>
#endif

#if WANT_PHYSFS
#include "physfs.h"
#endif

#include <stdlib.h>
#include <string.h>

// Game resources remain in gamedir; only writes use the persistent save root.
// Reject paths that could escape either root when games request filesystem I/O.
static bool fs_resolve(char *dest, size_t capacity, const char *root, const char *path)
{
   const char *segment = path;
   if (!root[0] || !path[0] || path[0] == '/' || path[0] == '\\')
      return false;
   for (const char *cursor = path; ; ++cursor)
   {
      if (*cursor == '\\')
         return false;
      if (*cursor == '/' || *cursor == '\0')
      {
         const size_t length = (size_t)(cursor - segment);
         if (!length || (length == 1 && segment[0] == '.') ||
             (length == 2 && segment[0] == '.' && segment[1] == '.'))
            return false;
         if (*cursor == '\0')
            break;
         segment = cursor + 1;
      }
   }
   const size_t root_length = strlen(root);
   const size_t path_length = strlen(path);
   if (root_length + path_length >= capacity)
      return false;
   memcpy(dest, root, root_length);
   memcpy(dest + root_length, path, path_length + 1);
   return true;
}

static bool fs_saved_path(char *dest, const char *path)
{
   return fs_resolve(dest, PATH_MAX_LENGTH, settings.savedir, path);
}

static bool fs_game_path(char *dest, const char *path)
{
   return fs_resolve(dest, PATH_MAX_LENGTH, settings.gamedir, path);
}

int _raw_get_user_writable_dir(lua_State *L)
{
   char* savedir;
   if ((*settings.environ_cb)(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &savedir)) {
      lua_pushstring(L, savedir);
      return 1;
   }

   // in the case that no savedir is available: return nil rather than seeking a fallback.
   return 0;
}

int lutro_filesystem_preload(lua_State *L)
{
   static const luaL_Reg fs_funcs[] =  {
      { "exists",      fs_exists },
      { "read",        fs_read },
      { "write",       fs_write },
      { "setRequirePath", fs_setRequirePath },
      { "getRequirePath", fs_getRequirePath },
      { "load",        fs_load },
      { "setIdentity", fs_setIdentity },
      { "getAppdataDirectory", fs_getAppdataDirectory },
      { "getWorkingDirectory", fs_getWorkingDirectory },
      { "isDirectory", fs_isDirectory },
      { "isFile",      fs_isFile },
      { "createDirectory", fs_createDirectory },
      { "getDirectoryItems", fs_getDirectoryItems },

      // internal helpers for lua-authored functions.
      { "_raw_get_user_writable_dir", _raw_get_user_writable_dir },
      { NULL, NULL }
   };

   lutro_newlib(L, fs_funcs, "filesystem");

   // lutro.filesystem._appendTrailingSlash - appends a trailing slash to match behavior of LOVE.
   // Because slash or backslash is platform dependent, this implementation takes the laziest
   // approach: if the incoming path has a trailing backslash then it checks if the OS environment
   // is Windows (WINDIR) and if yes then it doesn't append a trailing slash. It does not attempt
   // to append trialing backslashes. Windows tolerates forward slash and mixed-slash paths well.
   if (1) {
      int ret = luaL_dostring(L, "\
      lutro.filesystem._appendTrailingSlash = function(path)\n\
         if path:sub(-1) == '\\\\' then\n\
            local winDir = os.getenv('WINDIR')\n\
            if winDir ~= nil and winDir ~= '' then\n\
               path = path .. '/'\n\
            end\n\
         end\n\
         if path:sub(-1) ~= '/' then\n\
            path = path .. '/'\n\
         end\n\
         return path\n\
      end");

      if (ret) {
         // don't hard-fail - no need to block apps that might not use the function.
         lutro_errorf("failed to assign lutro.filesystem.getUserDirectory: %s\n", lua_tostring(L, -1));
         lua_pop(L, 1); // pop error message
      }
   }

   // lutro.filesystem.getUserDirectory
   //
   // Returns the writable save directory provided by libretro frontend. The directory
   // may or may not be sandboxed according to a current user, depending on the design
   // and capabilities of the platform OS. Funtion may return nil if the libretro frontend
   // does not provide a writable save directory.
   //
   // implementation notes:
   //  - UserDirectory should always have a trailing slash (as confirmed on Love2D)
   //  - Cache the UserDir to avoid costly re-calculation of env var lookups. UserDir and all env vars
   //    are static for the lifetime of the process.
   //  - https://love2d.org/wiki/love.filesystem.getUserDirectory

   if (1) {
      int ret = luaL_dostring(L, "\
      lutro.filesystem.getUserDirectory = function()\n\
         local savedir = lutro.filesystem._raw_get_user_writable_dir()\n\
         if savedir == nil or savedir == '' then\n\
            return nil\n\
         end\n\
         return lutro.filesystem._appendTrailingSlash(savedir)\n\
      end");

      if (ret) {
         // don't hard-fail - no need to block apps that might not use the function.
         lutro_errorf("failed to assign lutro.filesystem.getUserDirectory: %s\n", lua_tostring(L, -1));
         lua_pop(L, 1); // pop error message
      }
   }

   return 1;
}

void lutro_filesystem_init(void)
{
   #if WANT_PHYSFS
   PHYSFS_init(NULL);
   #endif
}

void lutro_filesystem_deinit(void)
{
   #if WANT_PHYSFS
   PHYSFS_deinit();
   #endif
}

/**
 * contents, size = lutro.filesystem.read(name, size)
 *
 * https://love2d.org/wiki/love.filesystem.read
 */
int fs_read(lua_State *L)
{
   const char *path = luaL_checkstring(L, 1);

   char fullpath[PATH_MAX_LENGTH];
   if (!fs_game_path(fullpath, path))
      return luaL_error(L, "Invalid filesystem path");
   if (settings.savedir[0])
   {
      char saved[PATH_MAX_LENGTH];
      if (fs_saved_path(saved, path) && filestream_exists(saved))
         strlcpy(fullpath, saved, sizeof(fullpath));
   }

   FILE *fp = fopen(fullpath, "rb");
   if (!fp)
      return 0;

   fseek(fp, 0, SEEK_END);
   long fsize = ftell(fp);
   fseek(fp, 0, SEEK_SET);

   char *string      = lutro_malloc(fsize + 1);
   size_t bytes_read = fread(string, 1, fsize, fp);
   fclose(fp);

   string[bytes_read] = 0;

   lua_pushlstring(L, string, bytes_read);
   lua_pushnumber(L, bytes_read);

   lutro_free(string);

   return 2;
}

int fs_write(lua_State *L)
{
   const char *path = luaL_checkstring(L, 1);
   size_t length = 0;
   const char *data = luaL_checklstring(L, 2, &length);

   char fullpath[PATH_MAX_LENGTH];
   if (!(settings.savedir[0] ? fs_saved_path(fullpath, path) : fs_game_path(fullpath, path)))
      return luaL_error(L, "Invalid filesystem path");

   FILE *fp = fopen(fullpath, "wb");
   if (!fp)
   {
      lua_pushboolean(L, 0);
      return 1;
   }

   const bool complete = fwrite(data, 1, length, fp) == length;
   const bool closed = fclose(fp) == 0;
   const bool written = complete && closed;

   lua_pushboolean(L, written);
   return 1;
}

/**
 * lutro.filesystem.setRequirePath
 *
 * @see https://love2d.org/wiki/love.filesystem.setRequirePath
 */
int fs_setRequirePath(lua_State *L)
{
   const char *path = luaL_checkstring(L, 1);
   lua_getglobal(L, "package");
   lua_pushstring(L, path) ;
   lua_setfield(L, -2, "path");
   lua_pop(L, 1);

   return 0;
}

/**
 * lutro.filesystem.getRequirePath
 *
 * @see https://love2d.org/wiki/love.filesystem.getRequirePath
 */
int fs_getRequirePath(lua_State *L)
{
   const char *cur_path;
   lua_getglobal(L, "package");
   lua_getfield(L, -1, "path");
   cur_path = lua_tostring( L, -1);
   lua_pop(L, 1);

   lua_pushstring(L, cur_path);

   return 1;
}

int fs_load(lua_State *L)
{
   const char *path = luaL_checkstring(L, 1);

   char fullpath[PATH_MAX_LENGTH];
   strlcpy(fullpath, settings.gamedir, sizeof(fullpath));
   strlcat(fullpath, path, sizeof(fullpath));

   FILE *fp = fopen(fullpath, "r");
   if (!fp)
      return -1;

   fseek(fp, 0, SEEK_END);
   long fsize = ftell(fp);
   fseek(fp, 0, SEEK_SET);

   char *string = lutro_malloc(fsize + 1);
   fread(string, fsize, 1, fp);
   fclose(fp);

   string[fsize] = 0;

   int status = luaL_loadbuffer(L, string, fsize, path);

   lutro_free(string);

   switch (status)
   {
   case LUA_ERRMEM:
      return luaL_error(L, "Memory allocation error: %s\n", lua_tostring(L, -1));
   case LUA_ERRSYNTAX:
      return luaL_error(L, "Syntax error: %s\n", lua_tostring(L, -1));
   default:
      return 1;
   }
}

int fs_exists(lua_State *L)
{
   const char *path = luaL_checkstring(L, 1);

   char fullpath[PATH_MAX_LENGTH];
   bool exists = fs_saved_path(fullpath, path) && filestream_exists(fullpath);
   if (!exists)
      exists = fs_game_path(fullpath, path) && filestream_exists(fullpath);

   lua_pushboolean(L, exists);
   return 1;
}

int fs_setIdentity(lua_State *L)
{
   const char *name = luaL_checkstring(L, 1);

   strlcpy(settings.identity, name, sizeof(settings.identity));

   return 0;
}

/**
 * lutro.filesystem.getAppdataDirectory
 *
 * Retrieves libretro's SYSTEM directory.
 *
 * @see https://love2d.org/wiki/love.filesystem.getAppdataDirectory
 * @see RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY
 */
int fs_getAppdataDirectory(lua_State *L)
{
   char* appdataDir;
   if ((*settings.environ_cb)(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &appdataDir)) {
      lua_pushstring(L, appdataDir);
   }
   else {
      lua_pushstring(L, "");
   }
   return 1;
}


/**
 * lutro.filesystem.getWorkingDirectory
 *
 * Retrieves the process current working directory (cwd or pwd). Lutro always returns the
 * relative directory prefix "./" which is generally interpreted by filesystem APIs as the
 * cwd when used as the prefix to a function. LOVE returns the CWD as an absolute path.
 * Note that the concept of cwd is inherently non-portable and leads to unexpected or surprising
 * behavior for users, and should only be used for local development or debug purposes.
 *
 * @see https://love2d.org/wiki/love.filesystem.getWorkingDirectory
 */
int fs_getWorkingDirectory(lua_State *L)
{
   lua_pushstring(L, "./");
   return 1;
}

int fs_isDirectory(lua_State *L)
{
   const char *path = luaL_checkstring(L, 1);

   char fullpath[PATH_MAX_LENGTH];
   bool res = fs_saved_path(fullpath, path) && path_is_directory(fullpath);
   if (!res)
      res = fs_game_path(fullpath, path) && path_is_directory(fullpath);

   lua_pushboolean(L, res);
   return 1;
}

int fs_isFile(lua_State *L)
{
   char fullpath[PATH_MAX_LENGTH];
   bool res         = false;
   const char *path = luaL_checkstring(L, 1);

   if (fs_saved_path(fullpath, path))
      res = filestream_exists(fullpath) && !path_is_directory(fullpath);
   if (!res && fs_game_path(fullpath, path))
      res = filestream_exists(fullpath) && !path_is_directory(fullpath);

   lua_pushboolean(L, res);
   return 1;
}

int fs_createDirectory(lua_State *L)
{
   bool res;
   char fullpath[PATH_MAX_LENGTH];
   const char *path = luaL_checkstring(L, 1);
   if (!(settings.savedir[0] ? fs_saved_path(fullpath, path) : fs_game_path(fullpath, path)))
      return luaL_error(L, "Invalid filesystem path");

   res = path_mkdir(fullpath);

   lua_pushboolean(L, res);
   return 1;
}

int fs_getDirectoryItems(lua_State *L)
{
   // Validate number of arguments.
   int n = lua_gettop(L);
   if (n != 1) {
      return luaL_error(L, "lutro.filesystem.getDirectoryItems requires 1 argument, %d given.", n);
   }

   const char *path = luaL_checkstring(L, 1);
   const char *roots[2] = { settings.savedir, settings.gamedir };
   lua_newtable(L);
   lua_newtable(L); // names already emitted, so save files shadow resources
   int index = 1;
   bool found = false;
   for (unsigned i = 0; i < 2; ++i)
   {
      if (!roots[i][0])
         continue;
      char fullpath[PATH_MAX_LENGTH];
      if (!*path)
         strlcpy(fullpath, roots[i], sizeof(fullpath));
      else if (!fs_resolve(fullpath, sizeof(fullpath), roots[i], path))
         return luaL_error(L, "Invalid filesystem path");
      if (!path_is_directory(fullpath))
         continue;
      found = true;
      libretro_vfs_implementation_dir *dir = retro_vfs_opendir_impl(fullpath, true);
      if (!dir)
         return luaL_error(L, "Failed to open the '%s' directory.", path);
      while (retro_vfs_readdir_impl(dir))
      {
         const char *name = retro_vfs_dirent_get_name_impl(dir);
         if (!name || string_is_equal(name, ".") || string_is_equal(name, ".."))
            continue;
         lua_getfield(L, -1, name);
         bool emitted = lua_toboolean(L, -1);
         lua_pop(L, 1);
         if (emitted)
            continue;
         lua_pushboolean(L, 1);
         lua_setfield(L, -2, name);
         lua_pushstring(L, name);
         lua_rawseti(L, -3, index++);
      }
      retro_vfs_closedir_impl(dir);
   }
   if (!found)
      return luaL_error(L, "The given directory of '%s' is not a directory.", path);
   lua_pop(L, 1); // seen names
   return 1;
}
