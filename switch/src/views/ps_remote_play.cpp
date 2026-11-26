#include "views/ps_remote_play.h"
PSRemotePlay::PSRemotePlay(Host *host)
	: host(host)
{
	this->io = IO::GetInstance();
}

void PSRemotePlay::willAppear(bool resetState)
{
	// borealis binds PLUS = quit app and MINUS = FPS counter on every view it
	// pushes. Inside a stream both keys are game buttons (Options / PS), so
	// swallow the app bindings and leave every mapping as it was.
	this->registerAction("", brls::Key::PLUS, [] { return true; }, true);
	this->registerAction("", brls::Key::MINUS, [] { return true; }, true);
}

void PSRemotePlay::draw(NVGcontext *vg, int x, int y, unsigned width, unsigned height, brls::Style *style, brls::FrameContext *ctx)
{
	this->io->MainLoop();
	this->host->SendFeedbackState();
}

PSRemotePlay::~PSRemotePlay()
{
}
