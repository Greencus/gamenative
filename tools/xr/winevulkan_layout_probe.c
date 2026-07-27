#include <stddef.h>
#include <stdio.h>

#include "vulkan_private.h"

int main(void)
{
    printf("device=%zu handle=%zu queue=%zu qhandle=%zu phys=%zu phandle=%zu\n",
           sizeof(struct wine_device), offsetof(struct wine_device, handle),
           sizeof(struct wine_queue), offsetof(struct wine_queue, handle),
           sizeof(struct wine_phys_dev), offsetof(struct wine_phys_dev, handle));
    return 0;
}
