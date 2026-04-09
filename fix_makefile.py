with open('Makefile', 'r') as f:
    content = f.read()
old = 'qemu-system-x86_64 '
new = 'qemu-system-x86_64 $(QEMU_NET) '
count = content.count(old)
content = content.replace(old, new)
with open('Makefile', 'w') as f:
    f.write(content)
print(f'replaced {count} occurrences')
