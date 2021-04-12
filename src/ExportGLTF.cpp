#include <cstdio>
#include <cstring>
#include <cassert>
#include <vector>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/filewritestream.h>


#include "Store.h"

namespace rj = rapidjson;

namespace {

  struct DataItem
  {
    DataItem* next = nullptr;
    const void* ptr = nullptr;
    uint32_t size = 0;
  };

  struct Context {
    rj::Document doc;

    rj::Value rjNodes = rj::Value(rj::kArrayType);

    uint32_t dataBytes = 0;
    ListHeader<DataItem> dataItems{};
    Arena arena;

    bool includeAttributes = true;

  };

  uint32_t addDataItem(Context* ctx, const void* ptr, size_t size, bool copy = false)
  {
    assert(ctx->dataBytes + size <= std::numeric_limits<uint32_t>::max());

    if (copy) {
      void* copied_ptr = ctx->arena.alloc(size);
      std::memcpy(copied_ptr, ptr, size);
      ptr = copied_ptr;
    }

    DataItem* item = ctx->arena.alloc<DataItem>();
    ctx->dataItems.insert(item);
    item->ptr = ptr;
    item->size = static_cast<uint32_t>(size);

    uint32_t offset = ctx->dataBytes;
    ctx->dataBytes += item->size;

    return offset;
  }

  uint32_t processGroup(Context* ctx, Group* group)
  {
    assert(group->kind == Group::Kind::Group);
    auto& alloc = ctx->doc.GetAllocator();

    rj::Value node(rj::kObjectType);
    if (group->group.name) {
      node.AddMember("name", rj::Value(group->group.name, alloc), alloc);
    }

    if (ctx->includeAttributes && group->attributes.first) {
      rj::Value extras(rj::kObjectType);
      
      for (Attribute* att = group->attributes.first; att; att = att->next) {
        extras.AddMember(rj::Value(att->key, alloc), rj::Value(att->val, alloc), alloc);
      }
      node.AddMember("extras", extras, alloc);
    }

    if (group->groups.first) {
      rj::Value children(rj::kArrayType);
      for (Group* child = group->groups.first; child; child = child->next) {
        children.PushBack(processGroup(ctx, child), alloc);
      }
      node.AddMember("children", children, alloc);
    }

    uint32_t index = ctx->rjNodes.Size();
    ctx->rjNodes.PushBack(node, alloc);

    return index;
  }

  void processModel(Context* ctx, std::vector<uint32_t>& siblings, Group* model)
  {
    assert(model->kind == Group::Kind::Model);
    for (Group* group = model->groups.first; group; group = group->next) {
      // Ignore recording file level, just recurse into nodes
      siblings.push_back(processGroup(ctx, group));
    }
  }

  void processFile(Context* ctx, std::vector<uint32_t>& siblings, Group* file)
  {
    assert(file->kind == Group::Kind::File);
    for (Group* model = file->groups.first; model; model = model->next) {
      // Ignore recording file level, just recurse into nodes
      processModel(ctx, siblings, model);
    }
  }


}


bool exportGLTF(Store* store, Logger logger, const char* path)
{

#ifdef _WIN32
  FILE* out = nullptr;
  auto err = fopen_s(&out, path, "wb");
  if (err != 0) {
    char buf[1024];
    if (strerror_s(buf, sizeof(buf), err) != 0) {
      buf[0] = '\0';
    }
    logger(2, "Failed to open %s for writing: %s", path, buf);
    return false;
  }
  assert(out);
#else
  FILE* out = fopen(path, "w");
  if (out == nullptr) {
    logger(2, "Failed to open %s for writing.", path);
    return false;
  }
#endif

  Context ctx;
  ctx.doc.SetObject();
  auto& alloc = ctx.doc.GetAllocator();


  rj::Value rjAsset( rj::kObjectType);
  rj::Value rjMeshes(rj::kArrayType);
  rj::Value rjAccessors(rj::kArrayType);
  rj::Value rjbufferViews(rj::kArrayType);
  rj::Value rjbuffers(rj::kArrayType);

  std::vector<uint32_t> rootNodes;
  for (Group* file = store->getFirstRoot(); file; file = file->next) {
    assert(file->kind == Group::Kind::File);
    processFile(&ctx, rootNodes, file);
  }

  rj::Value rjScenes(rj::kArrayType);
  {
    rj::Value rjSceneInstanceNodes(rj::kArrayType);
    for (uint32_t& index : rootNodes) {
      rjSceneInstanceNodes.PushBack(index, alloc);
    }

    rj::Value rjSceneInstance(rj::kObjectType);
    rjSceneInstance.AddMember("nodes", rjSceneInstanceNodes, alloc);
    rjScenes.PushBack(rjSceneInstance, alloc);
  }


  ctx.doc.AddMember("asset", rjAsset, ctx.doc.GetAllocator());
  ctx.doc.AddMember("scene", 0, ctx.doc.GetAllocator());
  ctx.doc.AddMember("scenes", rjScenes, ctx.doc.GetAllocator());
  ctx.doc.AddMember("nodes",  ctx.rjNodes, ctx.doc.GetAllocator());
  ctx.doc.AddMember("meshes", rjMeshes, ctx.doc.GetAllocator());
  ctx.doc.AddMember("accessors", rjAccessors, ctx.doc.GetAllocator());
  ctx.doc.AddMember("bufferViews", rjbufferViews, ctx.doc.GetAllocator());
  ctx.doc.AddMember("buffers", rjbuffers, ctx.doc.GetAllocator());




  addDataItem(&ctx, "test0", 5, false);
  addDataItem(&ctx, "test1", 5, true);

  // write header
  uint32_t header[3] = {
    0x46546C67,       // magic
    2,                // version
    12 + 8 + 8        // total length
  };
  if (fwrite(header, sizeof(header), 1, out) != 1) {
    logger(2, "%s: Error writing header", path);
    fclose(out);
    return false;
  }

  // write JSON chunk
  {
    rj::StringBuffer buffer;
    rj::Writer<rj::StringBuffer> writer(buffer);
    ctx.doc.Accept(writer);

    uint32_t json_chunk_header[2] = {
      static_cast<uint32_t>(buffer.GetSize()),  // length of chunk data
      0x4E4F534A                                // chunk type (JSON)
    };
    if (fwrite(json_chunk_header, sizeof(json_chunk_header), 1, out) != 1) {
      logger(2, "%s: Error writing JSON chunk header", path);
      fclose(out);
      return false;
    }
    if (fwrite(buffer.GetString(), buffer.GetSize(), 1, out) != 1) {
      logger(2, "%s: Error writing JSON data", path);
      fclose(out);
      return false;
    }
  }

  // Dump pretty-printed JSON to stdout for debugging
  if (true) {
    char writeBuffer[0x10000];
    rj::FileWriteStream os(stdout, writeBuffer, sizeof(writeBuffer));
    rj::PrettyWriter<rj::FileWriteStream> writer(os);
    writer.SetIndent(' ', 2);
    writer.SetMaxDecimalPlaces(4);
    ctx.doc.Accept(writer);
    putc('\n', stdout);
    fflush(stdout);
  }

  // write BIN chunk
  {

    uint32_t bin_chunk_header[2] = {
      ctx.dataBytes,  // length of chunk data
      0x004E4942      // chunk type (BIN)
    };
    if (fwrite(bin_chunk_header, sizeof(bin_chunk_header), 1, out) != 1) {
      logger(2, "%s: Error writing BIN chunk header", path);
      fclose(out);
      return false;
    }
    
    uint32_t offset = 0;
    for (DataItem* item = ctx.dataItems.first; item; item = item->next) {

      if (fwrite(item->ptr, item->size,  1, out) != 1) {
        logger(2, "%s: Error writing BIN chunk data at offset %u", path, offset);
        fclose(out);
        return false;
      }
      offset += item->size;
    }
    assert(offset = ctx.dataBytes);
  }

  fclose(out);
  return true;
}