#pragma once

// Shared, GL-free declarations for the editor's user-authored shader effects.
//
// The user drops ShaderToy-style GLSL fragment shaders (a `mainImage(out vec4,
// in vec2)` function) into a folder; each file is one selectable effect. A
// shader may declare tunable parameters via `//@param` annotation comments,
// which the Inspector turns into sliders/checkboxes. This header parses those
// annotations and wraps the user's snippet into a complete fragment shader. It
// deliberately pulls in no OpenGL so both the GUI and the export worker can use
// it; the actual rendering lives in ShaderRenderer.

#include <QColor>
#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

namespace harpia {

// One tunable uniform surfaced in the Inspector, declared in a shader via:
//   //@param <uniform> float <min> <max> <default> <Label...>
//   //@param <uniform> bool  <default(0|1)> <Label...>
struct ShaderParam {
	// Color travels in `def` (and in the params map) as a packed 0xAARRGGBB in
	// a double, exactly as PropType::Color does in the component model: 32 bits
	// are exact in a double, so one number carries every parameter kind and the
	// persistence, the keyframes and the uniforms all stay uniform.
	enum class Type { Float, Bool, Color };
	QString uniform;      // GLSL uniform name (also the JSON/persistence key)
	Type type = Type::Float;
	double min = 0.0;
	double max = 1.0;
	double def = 0.0;     // default value
	QString label;        // human label for the control (falls back to `uniform`)
};

// The active effect: which shader (file stem; empty == none) and the current
// value of each of its parameters. Persisted in the project and undoable.
struct ShaderState {
	QString name;                 // shader file stem, "" = no effect
	QMap<QString, double> params; // uniform name -> value

	bool operator==(const ShaderState &o) const { return name == o.name && params == o.params; }
	bool operator!=(const ShaderState &o) const { return !(*this == o); }
	bool active() const { return !name.isEmpty(); }
};

// Parse the `//@param` annotation lines out of a shader's source. Malformed
// lines are skipped. Order is preserved so the Inspector lists controls in the
// order they appear in the file.
inline QVector<ShaderParam> parseShaderParams(const QString &glsl)
{
	QVector<ShaderParam> out;
	const QStringList lines = glsl.split(QLatin1Char('\n'));
	for (const QString &raw : lines) {
		const QString line = raw.trimmed();
		if (!line.startsWith(QLatin1String("//@param")))
			continue;
		// Tokens after the "//@param" marker.
		const QStringList t =
			line.mid(8).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
		if (t.size() < 2)
			continue;
		ShaderParam p;
		p.uniform = t[0];
		const QString kind = t[1].toLower();
		if (kind == QLatin1String("float")) {
			// uniform float min max default [label...]
			if (t.size() < 5)
				continue;
			p.type = ShaderParam::Type::Float;
			p.min = t[2].toDouble();
			p.max = t[3].toDouble();
			p.def = t[4].toDouble();
			p.label = QStringList(t.mid(5)).join(QLatin1Char(' '));
		} else if (kind == QLatin1String("color")) {
			// uniform color #RRGGBB|#AARRGGBB [label...]
			//
			// Reaches the shader as a vec4 in 0..1, which is what a colour is
			// for in GLSL; the editor shows one swatch instead of three
			// sliders someone has to solve in their head.
			if (t.size() < 3)
				continue;
			p.type = ShaderParam::Type::Color;
			QColor c(t[2]);
			if (!c.isValid())
				c = QColor(Qt::white);
			p.min = 0.0;
			p.max = 0.0; // no range: a colour is not a scalar
			p.def = double(quint32(c.rgba()));
			p.label = QStringList(t.mid(3)).join(QLatin1Char(' '));
		} else if (kind == QLatin1String("bool")) {
			// uniform bool default [label...]
			if (t.size() < 3)
				continue;
			p.type = ShaderParam::Type::Bool;
			p.min = 0.0;
			p.max = 1.0;
			p.def = (t[2].toDouble() != 0.0) ? 1.0 : 0.0;
			p.label = QStringList(t.mid(3)).join(QLatin1Char(' '));
		} else {
			continue;
		}
		if (p.label.isEmpty())
			p.label = p.uniform;
		out.append(p);
	}
	return out;
}

// Wrap a ShaderToy-style snippet into a complete GLSL fragment shader: declare
// the standard channels (iResolution/iTime/iFrame/iChannel0) plus one uniform
// per parsed @param, then call the user's mainImage(). gl_FragCoord already has
// the ShaderToy pixel convention (origin bottom-left), so no remap is needed
// here — the renderer flips the sampled texture instead.
inline QString wrapShaderToy(const QString &userGlsl, const QVector<ShaderParam> &params)
{
	QString head;
	head += QStringLiteral("#version 330 core\n");
	head += QStringLiteral("out vec4 harpiaFragColor;\n");
	head += QStringLiteral("uniform vec3 iResolution;\n");
	head += QStringLiteral("uniform float iTime;\n");
	head += QStringLiteral("uniform int iFrame;\n");
	head += QStringLiteral("uniform sampler2D iChannel0;\n");
	for (const ShaderParam &p : params) {
		switch (p.type) {
		case ShaderParam::Type::Bool: head += QStringLiteral("uniform bool "); break;
		// vec4, not vec3: a colour parameter that cannot be faded is a colour
		// parameter someone immediately wants an opacity slider beside.
		case ShaderParam::Type::Color: head += QStringLiteral("uniform vec4 "); break;
		case ShaderParam::Type::Float: head += QStringLiteral("uniform float "); break;
		}
		head += p.uniform;
		head += QStringLiteral(";\n");
	}
	head += QStringLiteral("#line 1\n");

	QString tail;
	tail += QStringLiteral("\nvoid main(){ vec4 c = vec4(0.0); mainImage(c, gl_FragCoord.xy); "
			       "harpiaFragColor = c; }\n");
	return head + userGlsl + tail;
}

} // namespace harpia
