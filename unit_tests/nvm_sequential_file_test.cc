#ifdef ROCKSDB_PLATFORM_NVM

#include "nvm/nvm.h"

using namespace rocksdb;

int main(int argc, char **argv) {
  nvm_directory *dir;
  nvm *nvm_api;

  ALLOC_CLASS(nvm_api, nvm());
  ALLOC_CLASS(dir, nvm_directory("root", 4, nvm_api, nullptr));

  NVM_DEBUG("init complete");

  nvm_file *fd = dir->nvm_fopen("test.c", "w");

  NVM_DEBUG("file open at %p", fd);

  NVMSequentialFile *test_file = new NVMSequentialFile("test.c", fd, dir);

  unsigned long size = fd->GetSize();

  NVM_DEBUG("file has size %lu", size);

  if(!test_file->Skip(size / 4).ok()) {
    NVM_FATAL("skip failed");
  }

  NVM_DEBUG("Skip ok");

  Slice r;

  char *scratch;

  scratch = new char[size / 4 + 1];

  if(!test_file->Read(size / 4, &r, scratch).ok()) {
    NVM_FATAL("read test failed");
  }

  NVM_DEBUG("read ok");

  if(!test_file->Skip(size / 4).ok()) {
    NVM_FATAL("skip failed");
  }

  NVM_DEBUG("Skip 2 ok");

  if(!test_file->Read(size / 2, &r, scratch).ok()) {
    NVM_FATAL("read 2 test failed");
  }

  NVM_DEBUG("read 2 ok");
}

#else

int main(void) {
  return 0;
}

#endif
