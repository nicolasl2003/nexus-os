/* apk-split — découpe un paquet Alpine (.apk) ou un APKINDEX.tar.gz en ses
 * segments gzip successifs : 1.gz (signature), 2.gz (contrôle ou index),
 * 3.gz (données). Indispensable pour vérifier la signature, qui ne porte que
 * sur certains segments.        Usage : apk-split <fichier> <dossier>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "Usage : apk-split <fichier.apk> <dossier>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(size > 0 ? size : 1);
    if (!buf || fread(buf, 1, size, f) != (size_t)size) { fprintf(stderr, "apk-split : lecture impossible\n"); return 1; }
    fclose(f);
    static unsigned char out[65536];
    long off = 0;
    int seg = 0;
    while (off < size && seg < 8) {
        z_stream z;
        memset(&z, 0, sizeof z);
        if (inflateInit2(&z, 15 + 16) != Z_OK) return 1;
        z.next_in = buf + off;
        z.avail_in = (uInt)(size - off);
        int r;
        do {
            z.next_out = out;
            z.avail_out = sizeof out;
            r = inflate(&z, Z_NO_FLUSH);
        } while (r == Z_OK);
        if (r != Z_STREAM_END) { fprintf(stderr, "apk-split : archive gzip invalide\n"); return 1; }
        long used = (long)z.total_in;
        inflateEnd(&z);
        char path[4096];
        snprintf(path, sizeof path, "%s/%d.gz", argv[2], ++seg);
        FILE *o = fopen(path, "wb");
        if (!o || fwrite(buf + off, 1, used, o) != (size_t)used || fclose(o)) { perror(path); return 1; }
        off += used;
    }
    printf("%d\n", seg);
    return 0;
}
