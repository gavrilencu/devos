#include "fs.h"
#include "ata.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"

#define FS_BASE_LBA    2048u   /* zona FS incepe la 1 MiB in imaginea de disc */
#define TABLE_SECTORS  4u      /* regiune fixa pentru tabel (max 63 fisiere) */
#define FS_MAX_FILES   63
#define FS_MAX_SECTORS 22528u  /* zona FS are 11 MiB (imaginea totala: 12 MiB) */

static struct fs_file *files;  /* tabel in RAM, capacitate FS_MAX_FILES */
static int nfiles;
static int mounted;

static uint32_t sectors_of(uint32_t size)
{
    uint32_t s = (size + 511) / 512;
    return s ? s : 1;
}

void fs_init(void)
{
    uint8_t *tab = kmalloc(TABLE_SECTORS * 512);
    if (!tab)
        return;

    if (ata_read(FS_BASE_LBA, TABLE_SECTORS, tab) < 0) {
        kprintf("MyFS: eroare la citirea discului\n");
        kfree(tab);
        return;
    }
    if (memcmp(tab, "MYFS", 4) != 0) {
        kprintf("MyFS: superbloc lipsa (imagine fara sistem de fisiere?)\n");
        kfree(tab);
        return;
    }

    uint32_t n;
    memcpy(&n, tab + 4, 4);
    if (n > FS_MAX_FILES) {
        kprintf("MyFS: numar de fisiere suspect (%u)\n", n);
        kfree(tab);
        return;
    }

    files = kmalloc(FS_MAX_FILES * sizeof(struct fs_file));
    if (!files) {
        kfree(tab);
        return;
    }
    memcpy(files, tab + 8, n * sizeof(struct fs_file));
    kfree(tab);
    nfiles = (int)n;
    mounted = 1;

    kprintf("[ok] MyFS montat (read-write): %u fisiere\n", n);
}

/* Scrie superblocul + tabelul inapoi pe disc. */
static int write_table(void)
{
    static uint8_t buf[TABLE_SECTORS * 512];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "MYFS", 4);
    uint32_t n = (uint32_t)nfiles;
    memcpy(buf + 4, &n, 4);
    memcpy(buf + 8, files, (uint32_t)nfiles * sizeof(struct fs_file));
    return ata_write(FS_BASE_LBA, TABLE_SECTORS, buf);
}

int fs_count(void)
{
    return nfiles;
}

const struct fs_file *fs_get(int idx)
{
    if (idx < 0 || idx >= nfiles)
        return 0;
    return &files[idx];
}

const struct fs_file *fs_find(const char *name)
{
    for (int i = 0; i < nfiles; i++)
        if (strcmp(files[i].name, name) == 0)
            return &files[i];
    return 0;
}

void *fs_read_file(const char *name, uint32_t *size_out)
{
    const struct fs_file *f = fs_find(name);
    if (!f)
        return 0;

    uint32_t sectors = sectors_of(f->size);
    uint8_t *buf = kmalloc(sectors * 512);
    if (!buf)
        return 0;

    if (ata_read(FS_BASE_LBA + f->lba, sectors, buf) < 0) {
        kfree(buf);
        return 0;
    }

    if (size_out)
        *size_out = f->size;
    return buf;
}

int64_t fs_read_into(const char *name, void *dst, uint32_t maxlen)
{
    const struct fs_file *f = fs_find(name);
    if (!f)
        return -1;
    uint32_t sectors = sectors_of(f->size);
    if (sectors * 512u > maxlen)     /* citim sectoare intregi in dst */
        return -1;
    if (ata_read(FS_BASE_LBA + f->lba, sectors, dst) < 0)
        return -1;
    return (int64_t)f->size;
}

int fs_save(const char *name, const void *data, uint32_t size)
{
    if (!mounted || size == 0 || strlen(name) == 0 || strlen(name) > 23)
        return -1;

    uint32_t sectors = sectors_of(size);
    struct fs_file *f = (struct fs_file *)fs_find(name);

    if (f && sectors_of(f->size) >= sectors) {
        /* incape in sectoarele deja alocate: suprascriem pe loc */
    } else {
        /* alocam la capatul datelor existente */
        uint32_t lba = TABLE_SECTORS;
        for (int i = 0; i < nfiles; i++) {
            uint32_t end = files[i].lba + sectors_of(files[i].size);
            if (end > lba)
                lba = end;
        }
        if (lba + sectors > FS_MAX_SECTORS)
            return -2;                       /* disc plin */

        if (!f) {
            if (nfiles >= FS_MAX_FILES)
                return -3;                   /* tabel plin */
            f = &files[nfiles++];
            memset(f, 0, sizeof(*f));
            for (int i = 0; name[i] && i < 23; i++)
                f->name[i] = name[i];
        }
        f->lba = lba;
    }
    f->size = size;

    /* Scriem datele sector cu sector; ultimul (partial) e completat cu 0. */
    const uint8_t *p = data;
    for (uint32_t i = 0; i < sectors; i++) {
        uint8_t sec[512];
        uint32_t chunk = size - i * 512;
        if (chunk > 512)
            chunk = 512;
        memset(sec, 0, 512);
        memcpy(sec, p + i * 512, chunk);
        if (ata_write(FS_BASE_LBA + f->lba + i, 1, sec) < 0)
            return -1;
    }

    return write_table();
}

int fs_is_protected(const char *name)
{
    /* tot ce e in folderul de sistem "sys/" */
    if (name[0] == 's' && name[1] == 'y' && name[2] == 's' && name[3] == '/')
        return 1;
    /* programele si fisierele livrate de build */
    static const char *sys[] = {
        "ush", "calc", "edit", "basic", "show", "upper", "lines",
        "hello", "crash", "guess", "demo.bas", "readme.txt",
        "splash.raw", "desk.raw", 0,
    };
    for (int i = 0; sys[i]; i++)
        if (strcmp(name, sys[i]) == 0)
            return 1;
    return 0;
}

int fs_rename(const char *oldname, const char *newname)
{
    if (fs_is_protected(oldname))
        return -3;
    struct fs_file *f = (struct fs_file *)fs_find(oldname);
    if (!f)
        return -1;
    uint32_t nl = (uint32_t)strlen(newname);
    if (nl == 0 || nl > 23 || fs_find(newname))
        return -2;
    memset(f->name, 0, sizeof(f->name));
    memcpy(f->name, newname, nl);
    return write_table();
}

int fs_delete(const char *name)
{
    if (fs_is_protected(name))
        return -2;
    for (int i = 0; i < nfiles; i++) {
        if (strcmp(files[i].name, name) == 0) {
            memmove(&files[i], &files[i + 1],
                    (uint32_t)(nfiles - i - 1) * sizeof(struct fs_file));
            nfiles--;
            return write_table();
        }
    }
    return -1;
}
