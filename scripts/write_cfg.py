import struct

log_interval = int(.25 * 1000)
osr = 47
burst = 1

print(f"Log interval: {log_interval} ms")
print(f"OSR: {osr}")
print(f"Burst: {burst}")

file = open('FLUXGATE.CFG', "wb")
file.write(struct.pack('<l', log_interval))
file.write(struct.pack('<l', osr))
file.write(struct.pack('<l', burst))
file.close()
