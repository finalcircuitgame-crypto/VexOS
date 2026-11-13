#ifndef VEXOS_ERRNO_H
#define VEXOS_ERRNO_H
extern int *__errno_location(void);
#define errno (*__errno_location())
#define ENOENT 2
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENOMEM 12
#define EIO 5
#define EACCES 13
#define EROFS 30
#define ENOSYS 38
#endif
