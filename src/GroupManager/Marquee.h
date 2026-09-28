#pragma once

class WidgetManager;

// Резиновая рамка (marquee) по пустому месту рабочего стола: драг выбирает
// группы внутри рамки (как в проводнике), клик без движения — снимает
// выделение. Хук висит постоянно, но почти ничего не делает вне драга.
void MarqueeInit(WidgetManager* manager);
void MarqueeShutdown();
