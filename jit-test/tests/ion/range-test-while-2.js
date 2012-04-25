function f(b) {
	// We shouldn't need this check to get our results, but we do.
	if (b < 0) return;
	while (b < 200000000) {
		b++;
//		var y = b - 3;
	}
	print(b);
//	print(y);
}

for (var i = 0; i < 60; i++) {
	f(i);
}
