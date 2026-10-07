/*
	PSFlyCast - three more start-up animations: a part of bigpicture.cpp,
	which includes it (its helpers are that file's).

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Each ends as the first one does, with the mark and the name where the top
	bar has them, and each has its own way there:

	Spin up     the disc spins up out of the dark and stops on the sound's
	            hit; the name lands under it. The sign then glides to the top
	            left in one arc, the name swinging from under the mark round
	            to its right.
	Comet       a light winds in from the edge and strikes the middle on the
	            hit: the mark springs out of it and a glint writes the name.
	            Name and mark then fall back into the light, which loops away
	            to the top left and strikes there.
	Sound line  the start-up sound itself is a line across the screen, and
	            winds up into the disc's grooves on the hit; the name rises
	            from behind its own line. The splash then lifts like a
	            shutter, and what is left of it is the top bar.

	They are made to the start-up sound's moments (splashAt::Hit, Beat, Land),
	and what they share (the hit's light and rings, a ring that widens) the
	first animation, the birds, uses too. The designs are PSFlyCast's own.
*/

namespace
{

const char splashName[] = "PSFlyCast";

// A little past the end, and back.
float backOut(float t)
{
	t = std::clamp(t, 0.f, 1.f);
	const float a = 1.70158f, b = a + 1;
	return 1 + b * (t - 1) * (t - 1) * (t - 1) + a * (t - 1) * (t - 1);
}

float lerp(float a, float b, float p)
{
	return a + (b - a) * p;
}

// From one size to another as sizes go: by the same share each step.
float grown(float from, float to, float p)
{
	return from * std::pow(to / from, p);
}

// The mark's line (its own points, or others of the same lengths) from one
// share of its length to another: on a disc `radius` large at (cx, cy), turned
// by `angle`.
void strokeSpiralPart(ImDrawList *dl, const ImVec2 *points, float cx, float cy, float radius, float angle, float from,
		float to, ImU32 colour, float width, bool roundEnds = true)
{
	from = std::clamp(from, 0.f, 1.f);
	to = std::clamp(to, 0.f, 1.f);
	if (to <= from || (colour & IM_COL32_A_MASK) == 0)
		return;
	const SpiralLine& line = spiralLine();
	const float pixels = radius * S;
	const int stride = pixels > 160 ? 1 : pixels > 60 ? 2 : 4;
	const float c = std::cos(angle), s = std::sin(angle);
	auto place = [&](const ImVec2& p) {
		return V(cx + (p.x * c - p.y * s) * radius, cy + (p.x * s + p.y * c) * radius);
	};
	// The point a length along the line.
	auto cut = [&](float along) {
		int k = 0;
		while (k < SpiralLine::Count - 2 && line.along[k + 1] < along)
			k++;
		const float over = line.piece[k] > 0 ? std::clamp((along - line.along[k]) / line.piece[k], 0.f, 1.f) : 0;
		return ImVec2(points[k].x + (points[k + 1].x - points[k].x) * over, points[k].y + (points[k + 1].y - points[k].y) * over);
	};
	const float a = line.length * from, b = line.length * to;
	const ImVec2 first = place(cut(a)), last = place(cut(b));
	dl->PathClear();
	dl->PathLineTo(first);
	for (int k = 0; k < SpiralLine::Count; k += stride)
		if (line.along[k] > a && line.along[k] < b)
			dl->PathLineTo(place(points[k]));
	dl->PathLineTo(last);
	dl->PathStroke(colour, 0, width * S);
	if (roundEnds)
	{
		dl->AddCircleFilled(first, width * S / 2, colour, 12);
		dl->AddCircleFilled(last, width * S / 2, colour, 12);
	}
}

// The mark as these animations draw it: its body, its line's own light, and
// its line or a part of it.
void splashMark(ImDrawList *dl, const ImVec2 *points, float cx, float cy, float radius, float angle, float body, float light,
		float shown, float from = 0, float to = 1)
{
	discBody(dl, cx, cy, radius, col::text, col::accent, body);
	const float width = markWidth(radius);
	if (light > 0)
	{
		strokeSpiralPart(dl, points, cx, cy, radius, angle, from, to, alpha(col::accent, 0.10f * light * shown), width * 2.7f, false);
		strokeSpiralPart(dl, points, cx, cy, radius, angle, from, to, alpha(col::accent, 0.20f * light * shown), width * 1.7f, false);
	}
	strokeSpiralPart(dl, points, cx, cy, radius, angle, from, to, alpha(col::text, shown), width);
}

// A ring that widens and fades: x seconds into its life of `seconds`.
void splashRing(ImDrawList *dl, float cx, float cy, float from, float to, float x, float seconds, float a, float width,
		ImU32 colour)
{
	if (x < 0 || x > seconds)
		return;
	const float q = easeOut(x / seconds);
	dl->AddCircle(V(cx, cy), lerp(from, to, q) * S, alpha(colour, a * std::pow(1 - x / seconds, 1.5f)), 96, lerp(width, 1, q) * S);
}

// A short swell on each beat after the hit, until a moment: 0 to 1.
float splashBeat(float t, float until)
{
	float most = 0;
	for (float beat = splashAt::Hit + splashAt::Beat; beat < until; beat += splashAt::Beat)
		if (t >= beat)
			most = std::max(most, std::exp(-(t - beat) / 0.22f));
	return most;
}

// The hit, and the beats after it, around the disc in the middle.
void splashHit(ImDrawList *dl, float t, float cx, float cy, float until)
{
	const float x = t - splashAt::Hit;
	if (x >= 0 && x < 1)
		softDisc(dl, cx, cy, 560, alpha(col::text, 0.20f * std::exp(-x / 0.16f)), 64);
	splashRing(dl, cx, cy, 266, 640, x, 0.85f, 0.55f, 4, col::text);
	splashRing(dl, cx, cy, 266, 470, x - 0.07f, 0.7f, 0.25f, 2, col::accent);
	for (float beat = splashAt::Hit + splashAt::Beat; beat < until; beat += splashAt::Beat)
		splashRing(dl, cx, cy, 266, 372, t - beat, 0.55f, 0.24f, 2.5f, col::text);
}

// A turn from standing still that settles into the mark's own, x seconds on.
float splashTurn(float x)
{
	return x <= 0 ? 0 : MarkRate * (x - 0.6f * (1 - std::exp(-x / 0.6f)));
}

// How much of the user's own picture has taken the mark's place in the top
// bar, between two moments.
float ownLogo(float t, float from, float to)
{
	return customLogo() != ImTextureID() ? between(t, from, to) : 0;
}

// ------------------------------------------------------------- spin up

void drawSplashDock(ImDrawList *dl, float t)
{
	using namespace splashAt;
	const float begins = 0.25f, leaves = Hit + 4 * Beat;
	const float p = swell(between(t, leaves, Land)), q = 1 - p, x = t - Hit;
	// The mark's way to the top bar: one arc, by a point low on the left.
	const float cx = q * q * (W / 2 + DiscX) + 2 * q * p * 300 + p * p * MarkX;
	const float cy = q * q * DiscY + 2 * q * p * 452 + p * p * MarkY;
	const float size = grown(DiscRadius, MarkRadius, p);
	const float bump = x < 0 ? 0 : 0.05f * (x < 0.04f ? x / 0.04f : std::exp(-(x - 0.04f) / 0.13f));
	const float radius = size * (0.80f + 0.20f * easeOut(between(t, begins, Hit)))
			* (1 + bump + 0.012f * splashBeat(t, leaves - 0.1f) * (1 - p));
	// Its turn: fast, slower, still on the hit; then the mark's own.
	const float whole = Hit - begins, fastest = 34, left = std::clamp((Hit - t) / whole, 0.f, 1.f);
	const float angle = t < Hit ? -fastest * whole / 3 * left * left * left : markAngle();
	const float speed = t < Hit ? fastest * left * left : 0;

	const float lit = between(t, 0.2f, 1.6f) * (1 - p);
	if (lit > 0)
		softDisc(dl, cx, cy, 760, alpha(col::accent, 0.17f * lit), 64);
	if (p <= 0)
		splashHit(dl, t, cx, cy, leaves - 0.1f);
	const float own = ownLogo(t, Land + 0.06f, Land + 0.6f);
	if (t > begins)
	{
		const float share = swell(between(t, 0.3f, 1.9f)), shown = easeOut(between(t, begins, 0.7f)) * (1 - own);
		// The turn's blur: the line where it just was.
		for (int j = 7; j >= 1 && speed > 0.5f && share > 0.04f; j--)
			strokeSpiralPart(dl, spiralLine().at, cx, cy, radius, angle - j * speed * 0.011f, 0, share,
					alpha(col::text, shown * 0.34f * (1 - j / 8.f) * std::min(1.f, speed / 7)), markWidth(radius));
		splashMark(dl, spiralLine().at, cx, cy, radius, angle, between(x, 0, 0.3f) * (1 - own), 1 - p, shown, 0, share);
		if (own > 0)
			spinMark(dl, MarkX, MarkY, MarkRadius, own);
	}
	splashRing(dl, MarkX, MarkY, 27, 70, t - Land, 0.5f, 0.5f, 2.5f, col::text);

	// The name lands on the hit, and swings round the mark as the sign leaves:
	// its middle, from the mark's and in the mark's radii, is under it, then
	// to its right.
	if (x >= 0)
	{
		const float pn = swell(between(t, leaves + 0.05f, Land + 0.08f));
		const float fromY = (NameTop + NameSize / 2 - DiscY) / DiscRadius;
		const float toX = (BarX + textSize(bold(), BarSize, splashName).x / 2 - MarkX) / MarkRadius;
		const float toY = (BarTop + BarSize / 2 - MarkY) / MarkRadius;
		const float a0 = IM_PI / 2, a1 = std::atan2(toY, toX);
		const float way = a0 + (a1 - a0) * pn, far = grown(fromY, std::sqrt(toX * toX + toY * toY), pn) * size;
		const float letters = std::round(grown(NameSize / DiscRadius, BarSize / MarkRadius, pn) * size
				* (0.86f + 0.14f * backOut(between(x, 0, 0.38f))));
		const float mx = cx + std::cos(way) * far, my = cy + std::sin(way) * far;
		text(dl, bold(), letters, mx - textSize(bold(), letters, splashName).x / 2, my - letters / 2,
				alpha(col::text, between(x, 0, 0.15f)), splashName);
	}
}

// --------------------------------------------------------------- comet

// The light's way in: round the middle and into it, s from 0 to 1.
ImVec2 cometIn(float s)
{
	const float far = 1260 * std::pow(1 - s, 1.25f), round = 0.62f + 2 * IM_PI * 2.4f * std::pow(s, 1.45f);
	return ImVec2(W / 2 + splashAt::DiscX + far * std::cos(round), splashAt::DiscY + far * std::sin(round));
}

// And its way out to the top bar's mark, by a loop.
ImVec2 cometOut(float s)
{
	s = std::clamp(s, 0.f, 1.f);
	const float gone = std::pow(s, 2.7f), loop = 300 * (1 - s), from = 2.4f, round = from - 2 * IM_PI * 1.2f * std::pow(s, 0.9f);
	return ImVec2(lerp(W / 2 + splashAt::DiscX, splashAt::MarkX, gone) + loop * (std::cos(round) - std::cos(from)),
			lerp(splashAt::DiscY, splashAt::MarkY, gone) + loop * (std::sin(round) - std::sin(from)));
}

// The light's tail, from where it is (s) back by `span`: strokes one over
// another, the longer the narrower and the fainter.
template<typename Way>
void cometTail(ImDrawList *dl, Way&& way, float s, float span, float width, float a)
{
	if (a <= 0)
		return;
	const int passes = 10;
	for (int layer = 0; layer < 2; layer++)
		for (int j = passes; j >= 1; j--)
		{
			const float back = std::min(span * j / passes, s), thin = 1 - (j - 1) / (float)passes;
			if (back <= 0)
				continue;
			const int pieces = 3 + 2 * j;
			dl->PathClear();
			for (int i = 0; i <= pieces; i++)
			{
				const ImVec2 at = way(s - back * i / pieces);
				dl->PathLineTo(V(at.x, at.y));
			}
			if (layer == 0)
				dl->PathStroke(alpha(col::accent, a * 0.07f * thin), 0, (width * 2.6f * thin + 1) * S);
			else
				dl->PathStroke(alpha(col::text, a * (0.03f + 0.42f * std::pow(thin, 1.2f))), 0, (width * thin + 0.6f) * S);
		}
}

void cometHead(ImDrawList *dl, ImVec2 at, float a, float size)
{
	if (a <= 0)
		return;
	softDisc(dl, at.x, at.y, 86 * size, alpha(col::accent, 0.5f * a), 48);
	softDisc(dl, at.x, at.y, 26 * size, alpha(col::text, 0.9f * a), 32);
	dl->AddCircleFilled(V(at.x, at.y), 6.5f * size * S, alpha(col::text, a), 24);
}

// A name being written from its left edge, with the glint that writes it:
// how far, 0 to 1.
void writeName(ImDrawList *dl, float size, float left, float top, float written, float margin)
{
	if (written <= 0)
		return;
	const float wide = textSize(bold(), size, splashName).x;
	const float edge = left - margin + (wide + 2 * margin) * written;
	dl->PushClipRect(V(0, top - size * 0.2f), V(edge, top + size * 1.25f), true);
	text(dl, bold(), size, left, top, col::text, splashName);
	dl->PopClipRect();
	const float glint = 1 - between(written, 0.8f, 1), unit = size / 126;
	rect(dl, edge - 16 * unit, top + 4 * unit, 32 * unit, size - 8 * unit, alpha(col::accent, 0.16f * glint));
	rect(dl, edge - 6 * unit, top + 8 * unit, 12 * unit, size - 16 * unit, alpha(col::accent, 0.30f * glint));
	rect(dl, edge - std::max(1.f, 1.5f * unit), top + 10 * unit, std::max(2.f, 3 * unit), size - 20 * unit, alpha(col::text, glint));
}

void drawSplashComet(ImDrawList *dl, float t)
{
	using namespace splashAt;
	const float enters = 0.15f, falls = 4.50f, flies = Hit + 4.5f * Beat;
	const float x = t - Hit, cx = W / 2 + DiscX, cy = DiscY;
	const float lit = between(t, 0.4f, 2.0f) * (1 - between(t, 4.9f, 5.4f));
	if (lit > 0)
		softDisc(dl, cx, cy, 760, alpha(col::accent, 0.17f * lit), 64);

	// The light winds in; where it went stays a moment.
	if (t >= enters && t < Hit + 0.9f)
	{
		const float s = between(t, enters, Hit), fade = 1 - between(t, Hit, Hit + 0.8f);
		const int parts = 24, pieces = 6;
		for (int part = 0; part < parts; part++)
		{
			const float s0 = s * part / parts, s1 = s * (part + 1) / parts;
			const float a = 0.20f * fade * std::exp(-(s - (s0 + s1) / 2) * (Hit - enters) / 0.9f);
			if (a < 0.004f)
				continue;
			dl->PathClear();
			for (int i = 0; i <= pieces; i++)
			{
				const ImVec2 at = cometIn(lerp(s0, s1, i / (float)pieces));
				dl->PathLineTo(V(at.x, at.y));
			}
			dl->PathStroke(alpha(col::accent, a), 0, 2.5f * S);
		}
		if (t < Hit)
		{
			cometTail(dl, cometIn, s, 0.13f, 11, 1);
			cometHead(dl, cometIn(s), 1, 1);
		}
	}

	if (x >= 0 && t < flies + 0.05f)
	{
		splashHit(dl, t, cx, cy, falls - 0.2f);
		// The mark springs out of the middle, and at the end winds back into it.
		const float span = std::min(easeOut(between(x, 0, 0.5f)), 1 - easeIn(between(t, 4.72f, 5.17f)));
		const float radius = DiscRadius * (1 + 0.012f * splashBeat(t, falls));
		if (span > 0)
			splashMark(dl, spiralLine().at, cx, cy, radius, splashTurn(x) + 5.5f * easeIn(between(t, 4.6f, flies)),
					between(x, 0.1f, 0.5f) * (1 - between(t, 4.6f, 4.95f)), 1, 1, 0.5f - span / 2, 0.5f + span / 2);
		const float gathers = easeIn(between(t, 4.75f, flies));
		if (gathers > 0)
			softDisc(dl, cx, cy, 240, alpha(col::accent, 0.5f * gathers), 48);
		cometHead(dl, ImVec2(cx, cy), between(t, 4.75f, 5.1f), lerp(0.4f, 1, between(t, 4.75f, flies)));

		// The name: a glint writes it; later its letters fall into the middle.
		const float left = W / 2 - textSize(bold(), NameSize, splashName).x / 2;
		if (t < falls)
			writeName(dl, NameSize, left, NameTop, easeInOut(between(t, Hit + 0.13f, Hit + 0.82f)), 14);
		else
		{
			const int count = (int)sizeof(splashName) - 1;
			for (int i = 0; i < count; i++)
			{
				const std::string before(splashName, i), letter(1, splashName[i]);
				const float starts = falls + (count - 1 - i) * 0.04f, u = between(t, starts, starts + 0.36f);
				const float x0 = left + textSize(bold(), NameSize, before.c_str()).x + textSize(bold(), NameSize, letter.c_str()).x / 2;
				const float y0 = NameTop + NameSize * 0.5f, less = easeIn(u * 1.6f);
				const float size = std::round(NameSize * (1 - 0.4f * less));
				text(dl, bold(), size, x0 - textSize(bold(), size, letter.c_str()).x / 2, NameTop + (NameSize - size) * 0.62f - 26 * less,
						alpha(col::text, 1 - between(u, 0.05f, 0.5f)), letter.c_str());
				if (u > 0.12f && u < 1)
				{
					// What is left of the letter, on its way.
					auto way = [&](float v) {
						const float e = easeIn(v), back = 1 - e;
						const float mx = lerp(x0, cx, 0.35f) + (x0 < cx ? -70.f : 70.f), my = 610;
						return ImVec2(back * back * x0 + 2 * back * e * mx + e * e * cx, back * back * y0 + 2 * back * e * my + e * e * cy);
					};
					const float v = between(u, 0.12f, 1), a = between(u, 0.12f, 0.3f);
					cometTail(dl, way, v, 0.16f, 6, a);
					const ImVec2 at = way(v);
					dl->AddCircleFilled(V(at.x, at.y), lerp(5, 3, v) * S, alpha(col::text, a), 16);
				}
			}
		}
	}

	// The light loops away to the top left,
	if (t >= flies && t < Land)
	{
		const float s = between(t, flies, Land);
		softDisc(dl, cx, cy, 300, alpha(col::accent, 0.4f * (1 - between(s, 0, 0.35f))), 48);
		cometTail(dl, cometOut, s, 0.30f, 12, 1);
		cometHead(dl, cometOut(s), 1, lerp(1, 0.6f, s));
	}
	// and strikes there: the mark springs out, and the name is written beside it.
	const float y = t - Land;
	if (y >= 0)
	{
		if (y < 1.2f)
		{
			softDisc(dl, MarkX, MarkY, 190, alpha(col::accent, 0.55f * std::exp(-y / 0.2f)), 48);
			softDisc(dl, MarkX, MarkY, 70, alpha(col::text, 0.8f * std::exp(-y / 0.1f)), 32);
		}
		splashRing(dl, MarkX, MarkY, 24, 110, y, 0.55f, 0.6f, 3, col::text);
		splashRing(dl, MarkX, MarkY, 24, 70, y - 0.06f, 0.45f, 0.3f, 2, col::accent);
		const float span = easeOut(between(y, 0, 0.42f)), own = ownLogo(y, 0.42f, 0.88f);
		splashMark(dl, spiralLine().at, MarkX, MarkY, MarkRadius, markAngle(), between(y, 0.1f, 0.45f) * (1 - own), 0, 1 - own,
				0.5f - span / 2, 0.5f + span / 2);
		if (own > 0)
			spinMark(dl, MarkX, MarkY, MarkRadius, own);
		writeName(dl, BarSize, BarX, BarTop, easeInOut(between(y, 0.14f, 0.68f)), 5);
	}
}

// ---------------------------------------------------------- sound line

// The start-up sound's shape at the animation's moment: the same for a frame.
enum { WaveCount = 240, WaveStep = 8 };		// values, and units across the screen for each
const float *splashWave(float t)
{
	static float wave[WaveCount];
	static int frame = -1;
	if (frame != ImGui::GetFrameCount())
	{
		frame = ImGui::GetFrameCount();
		ps5::sound::startupWave(t, wave, WaveCount);
	}
	return wave;
}

float waveAt(const float *wave, float along)
{
	const float at = std::max(along, 0.f) / WaveStep;
	const int before = (int)at;
	return lerp(wave[before % WaveCount], wave[(before + 1) % WaveCount], at - before);
}

// The sound as a level line, from x0 to x1 at y.
void waveLine(ImDrawList *dl, const float *wave, float x0, float x1, float y, float height, ImU32 colour, float width)
{
	if ((colour & IM_COL32_A_MASK) == 0 || x1 <= x0)
		return;
	dl->PathClear();
	for (float x = x0; x < x1; x += 4)
		dl->PathLineTo(V(x, y + waveAt(wave, x) * height));
	dl->PathLineTo(V(x1, y + waveAt(wave, x1) * height));
	dl->PathStroke(colour, 0, width * S);
}

// How far the shutter is up, 0 to 1, and where its edge is.
float shutterUp(float t)
{
	return swell(between(t, splashAt::Hit + 4 * splashAt::Beat, splashAt::Land));
}

float shutterEdge(float t)
{
	return lerp(H, splashAt::BarHeight, shutterUp(t));
}

void drawSplashShutter(ImDrawList *dl, float t)
{
	using namespace splashAt;
	const float coils = 1.62f, lifts = Hit + 4 * Beat;
	const float p = shutterUp(t), edge = shutterEdge(t), x = t - Hit;
	const float *wave = splashWave(t);
	const int count = (int)sizeof(splashName) - 1;

	// What is above the edge is the splash still.
	dl->PushClipRect(V(0, 0), V(W, edge), true);
	// The mark keeps above the rising edge, and in what is left goes to its place.
	const float cx = lerp(W / 2 + DiscX, MarkX, swell(between(p, 0.10f, 1))), cy = edge * lerp(DiscY / H, 0.5f, p);
	const float bump = x < 0 ? 0 : 0.05f * (x < 0.04f ? x / 0.04f : std::exp(-(x - 0.04f) / 0.13f));
	const float radius = grown(DiscRadius, MarkRadius, p) * (1 + bump + 0.012f * splashBeat(t, lifts - 0.1f) * (1 - p));
	const float lit = between(t, 0.3f, 1.8f) * (1 - p);
	if (lit > 0)
		softDisc(dl, cx, cy, 760, alpha(col::accent, 0.17f * lit), 64);
	if (p <= 0)
		splashHit(dl, t, cx, cy, lifts - 0.1f);

	// The line: the sound, then the disc's grooves.
	const float coil = between(t, coils, Hit), loose = 1 - coil * coil * (3 - 2 * coil);
	const float shown = easeOut(between(t, 0.08f, 0.95f));
	if (shown > 0)
	{
		static ImVec2 points[SpiralLine::Count], moved[SpiralLine::Count];
		const SpiralLine& spiral = spiralLine();
		const ImVec2 *line = spiral.at;
		if (loose > 0)
		{
			// The sound moves the line, less as it winds up, and never far.
			uncoil(loose, points);
			const float much = 1000 / DiscRadius * std::pow(loose, 1.3f), most = 140 / DiscRadius;
			for (int i = 0; i < SpiralLine::Count; i++)
			{
				const ImVec2& a = points[std::max(0, i - 1)], & b = points[std::min((int)SpiralLine::Count - 1, i + 1)];
				const float dx = b.x - a.x, dy = b.y - a.y, length = std::max(std::sqrt(dx * dx + dy * dy), 1e-6f);
				// (not at its two ends, which are off the screen while it is straight)
				const float ends = std::min(1.f, std::min(i, SpiralLine::Count - 1 - i) / 24.f);
				const float d = std::clamp(waveAt(wave, spiral.along[i] * DiscRadius) * much, -most, most) * ends;
				moved[i] = ImVec2(points[i].x + dy / length * d, points[i].y - dx / length * d);
			}
			line = moved;
		}
		const float own = ownLogo(t, Land + 0.05f, Land + 0.55f);
		const float width = lerp(markWidth(radius), 4, loose), from = 0.5f - shown / 2, to = 0.5f + shown / 2;
		discBody(dl, cx, cy, radius, col::text, col::accent, between(x, 0, 0.3f) * (1 - own));
		strokeSpiralPart(dl, line, cx, cy, radius, markAngle(), from, to, alpha(col::accent, 0.10f * (1 - p)), width * 2.7f, false);
		strokeSpiralPart(dl, line, cx, cy, radius, markAngle(), from, to, alpha(col::accent, 0.20f * (1 - p)), width * 1.7f, false);
		strokeSpiralPart(dl, line, cx, cy, radius, markAngle(), from, to, alpha(col::text, 1 - own), width);
		if (own > 0)
			spinMark(dl, MarkX, MarkY, MarkRadius, own);
	}

	// The name rises from behind its line, a letter after another.
	if (x >= 0)
	{
		const float wide = textSize(bold(), NameSize, splashName).x, left = W / 2 - wide / 2;
		dl->PushClipRect(V(0, NameTop - 30), V(W, NameTop + NameSize + 6), true);
		for (int i = 0; i < count; i++)
		{
			const std::string before(splashName, i), letter(1, splashName[i]);
			const float u = easeOut(between(x, 0.02f + i * 0.035f, 0.44f + i * 0.035f));
			text(dl, bold(), NameSize, left + textSize(bold(), NameSize, before.c_str()).x, NameTop + (1 - u) * 128,
					alpha(col::text, between(u, 0, 0.25f)), letter.c_str());
		}
		dl->PopClipRect();
		// Its line: the sound, small.
		const float out = easeOut(between(x, 0.05f, 0.6f)), a = 1 - between(t, lifts - 0.25f, lifts + 0.1f);
		if (out > 0 && a > 0)
		{
			const float y = NameTop + NameSize + 14, half = wide / 2 * out;
			waveLine(dl, wave, W / 2 - half, W / 2 + half, y, 22, alpha(col::accent, 0.25f * a), 7);
			waveLine(dl, wave, W / 2 - half, W / 2 + half, y, 22, alpha(col::accent, a), 2.6f);
		}
	}
	dl->PopClipRect();

	// The shutter's edge: the sound line again, on its way up; then the bar's own edge.
	if (p > 0)
	{
		const float a = between(t, lifts, lifts + 0.12f) * (1 - between(t, Land + 0.1f, Land + 0.6f));
		const float height = 16 * (1 - 0.7f * p);
		waveLine(dl, wave, 0, W, edge, height, alpha(col::accent, 0.12f * a), 18);
		waveLine(dl, wave, 0, W, edge, height, alpha(col::accent, 0.30f * a), 8);
		waveLine(dl, wave, 0, W, edge, height, alpha(col::text, a), 2.4f);
	}
	splashRing(dl, MarkX, MarkY, 27, 70, t - Land, 0.5f, 0.5f, 2.5f, col::text);
	// The name in the bar rises the same way.
	if (t > 5.8f)
	{
		dl->PushClipRect(V(0, BarTop - 8), V(W, BarTop + BarSize + 4), true);
		for (int i = 0; i < count; i++)
		{
			const std::string before(splashName, i), letter(1, splashName[i]);
			const float u = easeOut(between(t, 5.84f + i * 0.03f, 6.14f + i * 0.03f));
			text(dl, bold(), BarSize, BarX + textSize(bold(), BarSize, before.c_str()).x, BarTop + (1 - u) * 40,
					alpha(col::text, between(u, 0, 0.25f)), letter.c_str());
		}
		dl->PopClipRect();
	}
}

}
