import struct

log_interval = int(.5 * 1000)
osr = 47

print(f"Log interval: {log_interval} ms")
print(f"OSR: {osr}")

file = open('FLUXGATE.CFG', "wb")
file.write(struct.pack('<l', log_interval))
file.write(struct.pack('<l', osr))
file.close()
