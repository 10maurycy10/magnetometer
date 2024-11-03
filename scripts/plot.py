from matplotlib import pyplot

readings = []

file = open("fluxgate.csv")

scaling = 48 * 5

for line in file:
	split = line.rstrip().split(',')
	try:
		int(split[0])
		readings += [int(split[1])/scaling]
	except:
		pass

pyplot.plot(readings)
pyplot.show()
