#pragma once

#include <uhepch.h>
#include "Event.h"




namespace UHE {

	class UHE_API WindowResizeEvent : public Event
	{


	public:
		WindowResizeEvent(unsigned int width, unsigned int height)
			: m_Width(width), m_Height(height) {
		}


		inline unsigned int GetWidth() const { return m_Width; }
		inline unsigned int GetHeight() const { return m_Height; }


		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "WindowResizeEvent: " << m_Width << ", " << m_Height;
			return ss.str();
		}
		EVENT_CLASS_TYPE(WindowResize)
		EVENT_CLASS_CATEGORY(EventCategoryApplication)


	private:
		unsigned int m_Width, m_Height;
	};
	class UHE_API WindowCloseEvent : public Event
	{
	public:
		WindowCloseEvent() {}

		EVENT_CLASS_TYPE(WindowClose)
        EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class UHE_API AppTickEvent : public Event
	{
	public:
		AppTickEvent() {};


		EVENT_CLASS_TYPE(AppTick)
		EVENT_CLASS_CATEGORY(EventCategoryApplication)

	};
	class UHE_API AppUpdateEvent : public Event
	{
	public:
		AppUpdateEvent() {}

		EVENT_CLASS_TYPE(AppUpdate) 
		EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class UHE_API AppRenderEvent : public Event
	{
	public:
		AppRenderEvent() {}

		EVENT_CLASS_TYPE(AppRender)
	    EVENT_CLASS_CATEGORY(EventCategoryApplication)

	};

	/**
	 * @brief Files dropped onto the window from the OS file explorer.
	 *
	 * Carries the dropped paths plus the cursor position at drop time, so a
	 * layer can decide which panel received the drop.
	 */
	class UHE_API WindowFileDropEvent : public Event
	{
	public:
		WindowFileDropEvent(std::vector<std::string> paths, float x, float y)
			: m_Paths(std::move(paths)), m_X(x), m_Y(y) {}

		const std::vector<std::string>& GetPaths() const { return m_Paths; }
		float GetX() const { return m_X; }
		float GetY() const { return m_Y; }

		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "WindowFileDropEvent: " << m_Paths.size() << " file(s) at " << m_X << ", " << m_Y;
			return ss.str();
		}

		EVENT_CLASS_TYPE(WindowFileDrop)
		EVENT_CLASS_CATEGORY(EventCategoryApplication | EventCategoryInput)

	private:
		std::vector<std::string> m_Paths;
		float m_X, m_Y;
	};

}
