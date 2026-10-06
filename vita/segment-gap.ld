/* vita-elf-create puts the module's SCE data after the code segment and needs
 * room for it before the data segment, which the default script starts at
 * the next 64 KB boundary. Without this the build fails ("Cannot allocate ...
 * bytes for SCE data at end of segment 0; segment 1 overlaps") whenever the
 * code happens to end within a few KB of such a boundary. */
SECTIONS
{
  . = . + 0x2000;
}
INSERT AFTER .eh_frame;
