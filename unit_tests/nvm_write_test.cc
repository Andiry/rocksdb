#ifdef ROCKSDB_PLATFORM_NVM

#include <iostream>
#include "nvm/nvm.h"

using namespace rocksdb;

void w_test_1()
{
  nvm_directory *dir;
  nvm *nvm_api;

  ALLOC_CLASS(nvm_api, nvm());
  ALLOC_CLASS(dir, nvm_directory("root", 4, nvm_api, nullptr));

  nvm_file *wfd = dir->nvm_fopen("test.c", "w");
  if(wfd == nullptr) {
    NVM_FATAL("");
  }

  nvm_file *srfd = dir->nvm_fopen("test.c", "r");
  if(srfd == nullptr) {
    NVM_FATAL("");
  }

  NVMWritableFile *w_file;
  NVMSequentialFile *sr_file;

  char data[100];
  char datax[200];

  Slice s, t;

  for (int i = 0; i < 100; ++i) {
    data[i] = i;
  }

  s = Slice(data, 100);

  ALLOC_CLASS(w_file, NVMWritableFile("test.c", wfd, dir));
  w_file->Append(s);

  w_file->Close();

  ALLOC_CLASS(sr_file, NVMSequentialFile("test2.c", srfd, dir));
  if (!sr_file->Read(100, &t, datax).ok()) {
    NVM_FATAL("");
  }

  size_t len = t.size();
  const char *data_read = t.data();

  if (len != 100) {
    NVM_FATAL("%lu", len);
  }

  for (int i = 0; i < 100; i++) {
    if (data_read[i] != data[i]) {
      NVM_FATAL("");
    }
  }

  delete w_file;

  NVM_DEBUG("TEST FINISHED!");
}

int main(int argc, char **argv)
{
  w_test_1();

  return 0;
}

#else // ROCKSDB_PLATFORM_NVM

int main(void)
{
  return 0;
}

#endif // ROCKSDB_PLATFORM_NVM