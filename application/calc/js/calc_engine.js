/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	calc_engine.js
 *	基本表計算: the cells' formulas worked out (design 11.13)
 *
 *	Loaded after jstat.min.js and formula.min.js, in an engine of the
 *	program's own. The program keeps what a cell holds as text, a
 *	formula when it starts with "="; it hands that text here, and the
 *	cells whose shown words changed come back. A formula is the
 *	expression after "=": every range (A1:B3) becomes the list of the
 *	numbers in it, every cell named ([row,col] or A1) becomes its value,
 *	a number as it is and anything else quoted, and what is left is
 *	evaluated as an expression with the functions of the table below. A
 *	cell that comes round to itself is #CIRCULAR!, and one that cannot
 *	be worked out #ERROR!. A cell's words are its value, or a formula's
 *	result, as text; a result that is 0, false or empty shows nothing.
 *
 *	calc.set(col, row, text), calc.clear(col, row), calc.blank(col, row),
 *	calc.move(cols, at, d) and calc.reset() change the cells; calc.bulk(true)
 *	and calc.bulk(false) go round many cells put in at once, which are
 *	worked out together at the end; calc.taken() hands over the
 *	cells changed since it was last asked, as records "col\x1Frow\x1F
 *	words" joined by \x1E. calc.test and calc.replace are the search's,
 *	a pattern in plain words or a regular expression.
 */

'use strict';

var calc = (function () {
	var cells = new Map();		/* "col,row" -> { value, formula, displayValue, refs, pass, dpass } */
	var changed = new Set();

	/*
	 * A working out is numbered: a formula worked out once in it keeps
	 * what it came to for the rest of it. While many cells are put in at
	 * once (bulk) nothing is worked out until they all are. Who names
	 * whom is made from the formulas' references when it is next wanted
	 * after a formula changed: the formulas naming each cell on its own,
	 * and the ranges formulas name.
	 */
	var pass = 1;
	var bulkOn = false;
	var namedBy = null;		/* "col,row" -> [ keys of formulas ] */
	var rangesNamed = null;		/* [ { c0, r0, c1, r1, k } ] */

	function letterToCol(letter) {
		var col = 0;
		for (var i = 0; i < letter.length; i++) {
			col = col * 26 + (letter.charCodeAt(i) - 64);
		}
		return col;
	}

	function colToLetter(col) {
		var letter = '';
		while (col > 0) {
			col--;
			letter = String.fromCharCode(65 + (col % 26)) + letter;
			col = Math.floor(col / 26);
		}
		return letter;
	}

	function key(col, row) {
		return col + ',' + row;
	}

	/* The value of a cell: a formula's worked out again, nothing coming round to itself */
	function valueWithRecalc(col, row, evaluating) {
		var k = key(col, row);
		var cell = cells.get(k);
		if (!cell) return '';
		if (cell.formula) {
			if (cell.pass === pass) return cell.displayValue;	/* worked out already in this one */
			var inner = new Set(evaluating);
			inner.add(k);
			var v = evaluate(cell.formula, col, row, inner);
			cell.displayValue = v;
			cell.pass = pass;
			return v;
		}
		return cell.value || '';
	}

	/* What a formula names, as ranges */
	function refsOf(f) {
		var out = [];
		if (!f) return out;
		f.replace(/([A-Z]+)(\d+):([A-Z]+)(\d+)/g, function (m, sc, sr, ec, er) {
			var c0 = letterToCol(sc), c1 = letterToCol(ec), r0 = parseInt(sr), r1 = parseInt(er);
			out.push({ c0: Math.min(c0, c1), r0: Math.min(r0, r1), c1: Math.max(c0, c1), r1: Math.max(r0, r1) });
			return m;
		});
		f.replace(/[\[［](\d+)[,，](\d+)[\]］]/g, function (m, r, c) {
			var rr = parseInt(r), cc = parseInt(c);
			out.push({ c0: cc, r0: rr, c1: cc, r1: rr });
			return m;
		});
		f.replace(/\b([A-Z]+)(\d+)\b/g, function (m, letters, rn) {
			var cc = letterToCol(letters), rr = parseInt(rn);
			out.push({ c0: cc, r0: rr, c1: cc, r1: rr });
			return m;
		});
		return out;
	}

	function buildNamed() {
		namedBy = new Map();
		rangesNamed = [];
		cells.forEach(function (cell, k) {
			if (!cell.refs) return;
			cell.refs.forEach(function (r) {
				if (r.c0 === r.c1 && r.r0 === r.r1) {
					var nk = key(r.c0, r.r0), list = namedBy.get(nk);
					if (list) list.push(k); else namedBy.set(nk, [k]);
				} else {
					rangesNamed.push({ c0: r.c0, r0: r.r0, c1: r.c1, r1: r.r1, k: k });
				}
			});
		});
	}

	function evaluate(formula, col, row, evaluating) {
		try {
			var expr = formula.substring(1).trim();

			/* a range: the numbers in it, as a list */
			expr = expr.replace(/([A-Z]+)(\d+):([A-Z]+)(\d+)/g, function (m, sc, sr, ec, er) {
				var c0 = letterToCol(sc), c1 = letterToCol(ec);
				var r0 = parseInt(sr), r1 = parseInt(er);
				var values = [];
				for (var r = r0; r <= r1; r++) {
					for (var c = c0; c <= c1; c++) {
						if (evaluating.has(key(c, r))) return '#CIRCULAR!';
						var v = valueWithRecalc(c, r, evaluating);
						if (v !== '' && !isNaN(Number(v))) values.push(Number(v));
					}
				}
				/* ", " keeps a list of two from reading as [row,col] */
				return '[' + values.join(', ') + ']';
			});

			/* [row,col], in half or full width */
			expr = expr.replace(/[\[［](\d+)[,，](\d+)[\]］]/g, function (m, r, c) {
				var rr = parseInt(r), cc = parseInt(c);
				if (evaluating.has(key(cc, rr))) return '#CIRCULAR!';
				var v = valueWithRecalc(cc, rr, evaluating);
				return isNaN(Number(v)) ? '"' + v + '"' : v;
			});

			/* A1 */
			expr = expr.replace(/\b([A-Z]+)(\d+)\b/g, function (m, letters, rn) {
				var cc = letterToCol(letters), rr = parseInt(rn);
				if (evaluating.has(key(cc, rr))) return '#CIRCULAR!';
				var v = valueWithRecalc(cc, rr, evaluating);
				return isNaN(Number(v)) ? '"' + v + '"' : v;
			});

			if (expr.includes('#CIRCULAR!')) return '#CIRCULAR!';

			var formulajs = globalThis.formulajs || {};
			var allowed = {
				SUM: formulajs.SUM || function () { return Array.prototype.slice.call(arguments).flat().reduce(function (a, b) { return a + b; }, 0); },
				AVERAGE: formulajs.AVERAGE || function () { var arr = Array.prototype.slice.call(arguments).flat(); if (arr.length === 0) return '#DIV/0!'; return arr.reduce(function (a, b) { return a + b; }, 0) / arr.length; },
				COUNT: formulajs.COUNT || function () { return Array.prototype.slice.call(arguments).flat().length; },
				MAX: formulajs.MAX || Math.max,
				MIN: formulajs.MIN || Math.min,
				IF: formulajs.IF || function (condition, t, f) { return condition ? t : f; },
				ROUND: formulajs.ROUND || Math.round,
				FLOOR: formulajs.FLOOR || Math.floor,
				CEIL: formulajs.CEILING || Math.ceil,
				ABS: formulajs.ABS || Math.abs,
				SQRT: formulajs.SQRT || Math.sqrt,
				POWER: formulajs.POWER || Math.pow,
				MOD: formulajs.MOD || function (a, b) { if (b === 0) return '#DIV/0!'; return a % b; },
				PI: formulajs.PI || function () { return Math.PI; },
				RAND: formulajs.RAND || Math.random,
				RANDBETWEEN: formulajs.RANDBETWEEN || function (min, max) { return Math.floor(Math.random() * (max - min + 1)) + min; }
			};

			/* names that would reach outside the expression are refused */
			var dangerous = /\b(require|import|process|window|document|global|globalThis|self|top|parent|frames|constructor|__proto__|prototype|eval|Function|setTimeout|setInterval|setImmediate|fetch|XMLHttpRequest|WebSocket|Worker|Blob|module|exports|child_process|fs|path|os|net|http|https|crypto)\b/;
			if (dangerous.test(expr)) return '#ERROR!';

			var names = Object.keys(allowed);
			var fns = names.map(function (n) { return allowed[n]; });
			var fn = Function.apply(null, names.concat(['"use strict"; return (' + expr + ');']));
			return fn.apply(null, fns);
		} catch (e) {
			return '#ERROR!';
		}
	}

	/* The words a cell shows */
	function shown(cell) {
		if (!cell) return '';
		var v = cell.displayValue || cell.value || '';
		return String(v);
	}

	function touch(col, row) {
		changed.add(key(col, row));
	}

	/*
	 * Every formula naming the cell worked out again, and the ones naming
	 * those, each once in a working out, one link after another
	 */
	function recalcDependents(col, row) {
		if (bulkOn) return;
		if (namedBy === null) buildNamed();
		var queue = [[col, row]];
		var one = function (k) {
			var cell = cells.get(k);
			if (!cell || !cell.formula || cell.dpass === pass) return;
			cell.dpass = pass;
			var p = k.split(',').map(Number);
			valueWithRecalc(p[0], p[1], new Set());
			touch(p[0], p[1]);
			queue.push(p);
		};
		for (var q = 0; q < queue.length; q++) {
			var c = queue[q][0], r = queue[q][1];
			(namedBy.get(key(c, r)) || []).forEach(one);
			rangesNamed.forEach(function (n) {
				if (c >= n.c0 && c <= n.c1 && r >= n.r0 && r <= n.r1) one(n.k);
			});
		}
	}

	function set(col, row, value) {
		var k = key(col, row);
		var cell = cells.get(k) || { value: '', formula: '', displayValue: '' };
		if (cell.refs && cell.refs.length) namedBy = null;
		if (!bulkOn) pass++;
		if (typeof value === 'string' && value.startsWith('=')) {
			cell.formula = value;
			cell.value = '';
			cell.refs = refsOf(value);
			namedBy = null;
			cell.displayValue = bulkOn ? '' : evaluate(value, col, row, new Set([k]));
			cell.pass = bulkOn ? 0 : pass;
		} else {
			cell.value = value;
			cell.formula = '';
			cell.refs = null;
			cell.displayValue = value;
		}
		cell.dpass = pass;
		cells.set(k, cell);
		touch(col, row);
		recalcDependents(col, row);
	}

	/* Many cells put in at once: worked out together at the end */
	function bulk(on) {
		if (on) {
			bulkOn = true;
			return;
		}
		bulkOn = false;
		pass++;
		cells.forEach(function (cell, k) {
			if (!cell.formula || cell.pass === pass) return;
			var p = k.split(',').map(Number);
			valueWithRecalc(p[0], p[1], new Set());
			touch(p[0], p[1]);
		});
	}

	/* A cell made empty where it is, nothing worked out again */
	function blank(col, row) {
		var cell = cells.get(key(col, row)) || { value: '', formula: '', displayValue: '' };
		if (cell.refs && cell.refs.length) namedBy = null;
		cell.value = '';
		cell.formula = '';
		cell.refs = null;
		cell.displayValue = '';
		cells.set(key(col, row), cell);
		touch(col, row);
	}

	function clear(col, row) {
		var cell = cells.get(key(col, row));
		if (cell && cell.refs && cell.refs.length) namedBy = null;
		cells.delete(key(col, row));
		touch(col, row);
		if (!bulkOn) {
			pass++;
			recalcDependents(col, row);	/* what named it names nothing now */
		}
	}

	/* A row or a column put in (+1) or taken out (-1) at 'at': the cells after it renamed, nothing worked out again */
	function move(isCol, at, d) {
		var next = new Map();
		cells.forEach(function (cell, k) {
			var p = k.split(',').map(Number);
			var n = isCol ? p[0] : p[1];
			if (d < 0 && n === at) return;
			if (n >= at && (d > 0 || n > at)) n += d;
			next.set(isCol ? key(n, p[1]) : key(p[0], n), cell);
		});
		cells = next;
		namedBy = null;
	}

	function taken() {
		var out = [];
		changed.forEach(function (k) {
			var p = k.split(',');
			out.push(p[0] + '\x1F' + p[1] + '\x1F' + shown(cells.get(k)));
		});
		changed.clear();
		return out.join('\x1E');
	}

	function pattern(pat, isRegex, flags) {
		return isRegex ? new RegExp(pat, flags) : null;
	}

	/* Whether text holds pat: 1, 0, or -1 for a pattern that is not one */
	function test(text, pat, isRegex) {
		try {
			var re = pattern(pat, isRegex, '');
			return (re ? re.test(text) : text.includes(pat)) ? 1 : 0;
		} catch (e) {
			return -1;
		}
	}

	function replace(text, pat, rep, isRegex) {
		try {
			if (isRegex) return text.replace(new RegExp(pat, 'g'), rep);
			return text.split(pat).join(rep);
		} catch (e) {
			return text;
		}
	}

	return {
		set: set,
		clear: clear,
		blank: blank,
		move: move,
		reset: function () { cells = new Map(); changed.clear(); namedBy = null; },
		bulk: bulk,
		taken: taken,
		shown: function (col, row) { return shown(cells.get(key(col, row))); },
		test: test,
		replace: replace
	};
})();
