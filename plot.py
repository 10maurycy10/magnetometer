from matplotlib import pyplot

readings = []

file = open("/mnt/FLUXGATE.CSV")

for line in file:
	split = line.rstrip().split(',')
	try:
		int(split[0])
		readings += [int(split[1])]
	except:
		pass

pyplot.plot(readings)
pyplot.show()
