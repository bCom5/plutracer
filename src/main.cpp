
#include "cmmn.h"
#include "texture.h"
#include "renderer.h"
#include "surfaces/surfaces.h"
#include "textures/textures.h"
#include "sampler.h"
#include "light.h"

#include "urn.h"
#include <fstream>

/*
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
!!
!!	This entire file is sketchy and subject to change
!!	It is entierly likely that many parts of it will eventually move to 
!!	their proper place. Additionaly this scene loading code is an abomination,
!!	and needs help. Also that group surface is... sketchy.
!!	
!!	Where to eval? who knows.... this needs major help
!!
!!	RIGHT NOW THE SCRIPTING/EXECUTION PORTION OF URN IS USED, BUT NOT EVERYWHERE
!!	This is because
!!	1) I don't know what to eval and what not to, 
!!		it can't yet be deterimined where it will be useful
!!	2) Namespacing issues related to what gets eval'd and in what eval_context
!!	3) urn has no std lib, not even basic functions like + or - or even a map, 
!!		so it is basically useless. Someone needs to go and define those
!!	RIGHT NOW: Scene loader evaluates basically everything in the same evaluation context
!!		Additionally it evaluates once the entire object value
!!
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
*/

namespace plu {
	// slow as possible, but for testing purposes whatever
	struct group : public plu::surface {
		vector<shared_ptr<surface>> surfaces;

		group(const vector<shared_ptr<surface>>& s) : surfaces(s) {}
		group(initializer_list<shared_ptr<surface>> s) : surfaces(s.begin(), s.end()) {}

		bool hit(const ray& r, hit_record* hr) const override {
			bool ah = false;
			hit_record mhr = hr != nullptr ? *hr : hit_record();
			for (const auto& s : surfaces) {
				hit_record phr = mhr;
				if (s->hit(r, &phr)) {
					ah = true;
					mhr = phr;
				}
			}
			if (hr != nullptr) *hr = mhr;
			return ah;
		}

		aabb bounds() const override {
			aabb tb;
			for (const auto& s : surfaces) tb.add_aabb(s->bounds());
			return tb;
		}
	};

	//this needs help
	struct postprocesser {
		vec3 whitePreservingLumaBasedReinhardToneMapping(vec3 color)
		{
			float white = 2.f;
			float luma = dot(color, vec3(0.2126f, 0.7152f, 0.0722f));
			float toneMappedLuma = luma * (1.f + luma / (white*white)) / (1.f + luma);
			color *= toneMappedLuma / luma;
			color = pow(color, vec3(1.f / 2.2f));
			return color;
		}

		void postprocess(shared_ptr<texture2d> rt) {
			vector<thread> workers; mutex scanline_mutex; uint32_t next_scanline = 0;
			for (int i = 0; i < thread::hardware_concurrency(); ++i) {
				workers.push_back(thread([&]{
					while (true) {
						uint32_t scanline;
						scanline_mutex.lock();
						{
							scanline = next_scanline++;
							if (scanline >= rt->size.y) {
								scanline_mutex.unlock();
								break;
							}
						}
						scanline_mutex.unlock();
						for (uint32_t x = 0; x < rt->size.x; ++x) {
							auto c = rt->pixel(uvec2(x, scanline));
							rt->pixel(uvec2(x, scanline)) = whitePreservingLumaBasedReinhardToneMapping(c);
						}
					}
				}));
			}
			for (auto& t : workers) t.join();
		}
	};
}

inline vec3 bk2v3(const urn::value& v) {
	return vec3(v[0].get_num(), v[1].get_num(), v[2].get_num());
}

inline vec3 bk2v3(urn::eval_context& cx, const urn::value& v) {
	auto rv = cx.reduce(v);
	return vec3(rv[0].get_num(), rv[1].get_num(), rv[2].get_num());
}

/*
Example of a scene definition:

resolution: [1280 960]
antialiasing: 16
camera: [
	position:	[0 0 -5]
	target:		[0 0 0]
	up:			[0 1 0]
]
materials: [
	green: [ diffuse [0.1 0.7 0.2] ]
	red: [ diffuse [0.7 0.2 0.1] ]
	checkerboard: [ diffuse texture [ checkerboard [0 0 0] [1 1 1] 8 ] ]
]
objects: do [
	concat [
		box [0 0 0] [10 0.1 10] 'checkerboard
	] [
		sphere [1.4 1.1 0] 1 'green
		sphere [-1.4 1.1 0] 1 'red
	]
]
*/

/*
	+ more lights
		- area lights
		- inf area lights
	+ more materials
		- Cook-Torrance
		- Ashikhmin&Shirley
		- Disney
	+ real stratified samples
	+ fix the box so that it can be used on all sides
	+ speed
*/

inline plu::props::color make_color(urn::eval_context& cx, const vector<urn::value>& vs, int& i) {
	if (vs[i].type == urn::value::Var) {
		if (vs[i].get_var() == "texture") {
			i++;
			auto ts = vs[i].get<vector<urn::value>>(); i++;
			auto t = ts[0].get_var();
			if (t == "checkerboard") {
				return plu::props::color(
					make_shared<plu::textures::checkerboard_texture>(bk2v3(cx, ts[1]), bk2v3(cx, ts[2]), cx.eval(ts[3]).get_num()));
			}
			else if (t == "grid") {
				return plu::props::color(
					make_shared<plu::textures::grid_texture>(bk2v3(cx, ts[1]), bk2v3(cx, ts[2]), cx.eval(ts[3]).get_num(), cx.eval(ts[4]).get<double>()));
			}
			else if (t == "img") {
				return plu::props::color(
					make_shared<plu::texture2d>(ts[1].get<string>()));
			}
			else throw;

		} else throw;
	}
	else if (vs[i].type == urn::value::Block) {
		return plu::props::color(bk2v3(cx, vs[i++]));
	}
	else throw;
}

inline shared_ptr<plu::material> make_material(urn::eval_context& cx, const urn::value& v) {
	auto vs = v.get<vector<urn::value>>();
	if (vs[0].get_var() == "diffuse") {
		int i = 1;
		return make_shared<plu::materials::diffuse_material>(make_color(cx, vs, i));
	}
	else if (vs[0].get_var() == "perfect-reflection") {
		int i = 1;
		auto c = make_color(cx, vs, i);
		auto e = bk2v3(cx, vs[i++]), k = bk2v3(cx, vs[i++]);
		return make_shared<plu::materials::perfect_reflection_material>(c,e,k);
	}
	else if (vs[0].get_var() == "perfect-refraction") {
		int i = 1;
		auto c = make_color(cx, vs, i);
		return make_shared<plu::materials::perfect_refraction_material>(c, vs[i].get_num(), vs[i + 1].get_num());
	}
	else if (vs[0].get_var() == "glass") {
		int i = 1;
		auto c = make_color(cx, vs, i);
		return make_shared<plu::materials::glass_material>(c, vs[i].get_num());
	}
	else throw;
}

int main(int argc, char* argv[]) {
	vector<string> args; for (int i = 1; i < argc; ++i) args.push_back(argv[i]);

	// !! we should probably move this code somewhere that is not main, but otoh it's kinda nice to have it here
	// !! also where else would it go anyways?

	// ***** this is still preliminary, and will probably change significatly *****
	int argi = 0;

	// a little silly REPL loop so that it's easier to test urn
	// perhaps one day this will happen after it loads the scene so that you can modify that
	if(args[argi] == "/i") {
		argi++;
		urn::eval_context cx;
		cx.create_std_funcs();
		while (true) {
			string s;
			cout << "urn> ";
			getline(cin, s);
			auto ts = urn::token_stream(istringstream(s));
			auto v = urn::value(ts);
			if (v.type == urn::value::Val) {
				auto cmd = v.get_val();
				if (cmd == "!q") break;
				else if (cmd == "!x") return 42;
			}
			cout << cx.eval(v) << endl;
		}
	}

	auto init_start = chrono::high_resolution_clock::now();

	auto ts = urn::token_stream(ifstream(args[argi]));
	urn::value scene_tlv = urn::value(ts); //requires first cmdline argument to be path to scene file for now

	auto resolu_b = scene_tlv.named_block_val("resolution");
	auto resolution = resolu_b.is_null() ? uvec2(1280, 960) : uvec2(resolu_b[0].get<int64_t>(), resolu_b[1].get<int64_t>());
	shared_ptr<plu::texture2d> tx = make_shared<plu::texture2d>( resolution );

	auto cam_b = scene_tlv.named_block_val("camera");
	vec2 dof_settings;
	if (cam_b.has_block_val_named("lens")) {
		auto lens_b = cam_b.named_block_val("lens");
		dof_settings.x = lens_b.named_block_val("radius").get_num();
		dof_settings.y = lens_b.named_block_val("focal-distance").get_num();
	}
	auto cam = plu::camera(bk2v3(cam_b.named_block_val("position")), bk2v3(cam_b.named_block_val("target")),
		tx->size, dof_settings.x, dof_settings.y);

	auto smp_cnt = scene_tlv.named_block_val("antialiasing-samples").get<int64_t>();
	
	map<string, shared_ptr<plu::material>> mats;
	vector<shared_ptr<plu::surface>> surfs;
	vector<shared_ptr<plu::light>> lights;


	urn::eval_context cx;
	cx.create_std_funcs();

	auto matvs = scene_tlv.named_block_val("materials").get<vector<urn::value>>();
	for (int i = 0; i < matvs.size(); ++i) {
		if (matvs[i].type != urn::value::Def) throw;
		auto d = matvs[i].get<pair<string, urn::value>>();
		mats[d.first] = make_material(cx, d.second);
	}


	auto objvs = cx.eval1(scene_tlv.named_block_val("objects")).get<vector<urn::value>>();
	for (int i = 0; i < objvs.size();) {
		auto ot = objvs[i].get_var();
		if (ot == "sphere") {
			auto m = objvs[i + 3];
			auto M = m.type == urn::value::Block ? make_material(cx, m) : m.type == urn::value::Id ? mats[m.get_id()] : nullptr;
			surfs.push_back(make_shared<plu::surfaces::sphere>(bk2v3(cx, objvs[i+1]), cx.eval(objvs[i+2]).get_num(), M));
			i += 4;
		}
		else if (ot == "box") {
			auto m = objvs[i + 3];
			auto M = m.type == urn::value::Block ? make_material(cx, m) : m.type == urn::value::Id ? mats[m.get_id()] : nullptr;
			surfs.push_back(make_shared<plu::surfaces::box>(bk2v3(cx, objvs[i + 1]), bk2v3(cx, objvs[i + 2]), M));
			i += 4;
		}
		else if (ot == "point-light") {
			lights.push_back(make_shared<plu::lights::point_light>(bk2v3(cx,objvs[i+1]),bk2v3(cx,objvs[i+2])));
			i += 3;
		}
	}

	auto s = //new plu::group(surfs);
		new plu::surfaces::bvh_tree(surfs);
	auto sampler = new plu::samplers::stratified_sampler(tx->size, uvec2(smp_cnt), true);
	plu::renderer r(s, cam, uvec2(32), sampler);
	r.lights = lights;
	
	auto init_end = chrono::high_resolution_clock::now();

	auto render_start = chrono::high_resolution_clock::now();
	r.render(tx);
	auto render_end = chrono::high_resolution_clock::now();

	auto pp_start = chrono::high_resolution_clock::now();
	plu::postprocesser pp;
	pp.postprocess(tx);
	auto pp_end = chrono::high_resolution_clock::now();

	auto init_time = chrono::duration_cast<chrono::milliseconds>(init_end - init_start); // convert to milliseconds b/c humans are bad at big numbers
	auto render_time = chrono::duration_cast<chrono::milliseconds>(render_end - render_start);
	auto pp_time = chrono::duration_cast<chrono::milliseconds>(pp_end - pp_start);
	ostringstream watermark;
	watermark
		<< "scene: " << args[0] << endl
		<< "init took: " << init_time.count() << "ms" << endl
		<< "render took: " << render_time.count() << "ms" << endl
		<< "postprocess took: " << pp_time.count() << "ms" << endl;
#ifdef _DEBUG
	watermark << "DEBUG" << endl;
#else
	watermark << "RELEASE" << endl;
#endif
	cout << watermark.str();

	tx->draw_text(watermark.str(), uvec2(9, 10), vec3(0.2f)); // make a snazzy drop shadow
	tx->draw_text(watermark.str(), uvec2(8, 8), vec3(1.f, 0.6f, 0)); //draw some text
	
	// generate a somewhat unique filename so that we can see our progress
	ostringstream fns;
	fns << "image_" << chrono::system_clock::now().time_since_epoch().count() << ".bmp";
	tx->write_bmp(fns.str()); //write to image.bmp
	getchar();

	delete s, sampler;

	return 0;
}
