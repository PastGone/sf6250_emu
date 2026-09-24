#include "rxi-log/log.h"
#include "mtk_boot_image.h"

#include <stdlib.h>

#define DEFAULT_IMAGE \
	"res/AHONG50_11B_PCB01_gprs_MT6250_S00.D27D__11B_W11_32__V0010.bin"

int main(int argc, char **argv) {
  const char *path = (argc > 1) ? argv[1] : DEFAULT_IMAGE;
  struct mtk_boot_image img;
  uint8_t *buf = NULL;
  size_t len = 0;
  int rc;

  rc = mtk_boot_image_load_file(path, &buf, &len);
  if (rc != MTK_IMG_OK) {
    log_error("failed to load \"%s\" (rc=%d)", path, rc);
    return 1;
  }

  rc = mtk_boot_image_parse(buf, len, &img);
  if (rc != MTK_IMG_OK) {
    log_error("failed to parse \"%s\" (rc=%d)", path, rc);
    free(buf);
    return 1;
  }

  log_info("parsed \"%s\"", path);
  mtk_boot_image_dump(&img);

  free(buf);
  return 0;
}
