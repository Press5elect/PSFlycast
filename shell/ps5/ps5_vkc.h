/*
	PSFlyCast - the Vulkan calls of the vendored frame generators.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	ps5-framegen and MihawkGen call Vulkan by its C names (vkCmdDispatch ...).
	The title has no loader to link them to: it reaches the driver through
	vulkan.hpp's dispatcher. Built with this header (-include), their calls
	go through the table `ps5vkc` instead, which ps5_framegen_ext.cpp fills
	from the driver once the device exists (ps5vkc_load).

	The file that fills the table defines PS5_VKC_NO_MACROS first: it also
	includes vulkan.hpp, whose own member names must stay as they are.
*/
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

// Every function the two generators call.
#define PS5_VKC_FUNCTIONS(X) \
	X(vkAllocateDescriptorSets) X(vkAllocateMemory) X(vkBindBufferMemory) X(vkBindImageMemory) \
	X(vkCmdBindDescriptorSets) X(vkCmdBindPipeline) X(vkCmdClearColorImage) X(vkCmdDispatch) X(vkCmdFillBuffer) \
	X(vkCmdPipelineBarrier) X(vkCmdPushConstants) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkCreateBuffer) \
	X(vkCreateComputePipelines) X(vkCreateDescriptorPool) X(vkCreateDescriptorSetLayout) X(vkCreateImage) \
	X(vkCreateImageView) X(vkCreatePipelineLayout) X(vkCreateQueryPool) X(vkCreateSampler) X(vkCreateShaderModule) \
	X(vkDestroyBuffer) X(vkDestroyDescriptorPool) X(vkDestroyDescriptorSetLayout) X(vkDestroyImage) \
	X(vkDestroyImageView) X(vkDestroyPipeline) X(vkDestroyPipelineLayout) X(vkDestroyQueryPool) X(vkDestroySampler) \
	X(vkDestroyShaderModule) X(vkFreeMemory) X(vkGetBufferMemoryRequirements) X(vkGetDeviceProcAddr) \
	X(vkGetImageMemoryRequirements) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties) \
	X(vkGetQueryPoolResults) X(vkMapMemory) X(vkUnmapMemory) X(vkUpdateDescriptorSets)

struct Ps5Vkc
{
#define X(name) PFN_##name name;
	PS5_VKC_FUNCTIONS(X)
#undef X
};

extern Ps5Vkc ps5vkc;

#ifndef PS5_VKC_NO_MACROS
#define vkAllocateDescriptorSets ps5vkc.vkAllocateDescriptorSets
#define vkAllocateMemory ps5vkc.vkAllocateMemory
#define vkBindBufferMemory ps5vkc.vkBindBufferMemory
#define vkBindImageMemory ps5vkc.vkBindImageMemory
#define vkCmdBindDescriptorSets ps5vkc.vkCmdBindDescriptorSets
#define vkCmdBindPipeline ps5vkc.vkCmdBindPipeline
#define vkCmdClearColorImage ps5vkc.vkCmdClearColorImage
#define vkCmdDispatch ps5vkc.vkCmdDispatch
#define vkCmdFillBuffer ps5vkc.vkCmdFillBuffer
#define vkCmdPipelineBarrier ps5vkc.vkCmdPipelineBarrier
#define vkCmdPushConstants ps5vkc.vkCmdPushConstants
#define vkCmdResetQueryPool ps5vkc.vkCmdResetQueryPool
#define vkCmdWriteTimestamp ps5vkc.vkCmdWriteTimestamp
#define vkCreateBuffer ps5vkc.vkCreateBuffer
#define vkCreateComputePipelines ps5vkc.vkCreateComputePipelines
#define vkCreateDescriptorPool ps5vkc.vkCreateDescriptorPool
#define vkCreateDescriptorSetLayout ps5vkc.vkCreateDescriptorSetLayout
#define vkCreateImage ps5vkc.vkCreateImage
#define vkCreateImageView ps5vkc.vkCreateImageView
#define vkCreatePipelineLayout ps5vkc.vkCreatePipelineLayout
#define vkCreateQueryPool ps5vkc.vkCreateQueryPool
#define vkCreateSampler ps5vkc.vkCreateSampler
#define vkCreateShaderModule ps5vkc.vkCreateShaderModule
#define vkDestroyBuffer ps5vkc.vkDestroyBuffer
#define vkDestroyDescriptorPool ps5vkc.vkDestroyDescriptorPool
#define vkDestroyDescriptorSetLayout ps5vkc.vkDestroyDescriptorSetLayout
#define vkDestroyImage ps5vkc.vkDestroyImage
#define vkDestroyImageView ps5vkc.vkDestroyImageView
#define vkDestroyPipeline ps5vkc.vkDestroyPipeline
#define vkDestroyPipelineLayout ps5vkc.vkDestroyPipelineLayout
#define vkDestroyQueryPool ps5vkc.vkDestroyQueryPool
#define vkDestroySampler ps5vkc.vkDestroySampler
#define vkDestroyShaderModule ps5vkc.vkDestroyShaderModule
#define vkFreeMemory ps5vkc.vkFreeMemory
#define vkGetBufferMemoryRequirements ps5vkc.vkGetBufferMemoryRequirements
#define vkGetDeviceProcAddr ps5vkc.vkGetDeviceProcAddr
#define vkGetImageMemoryRequirements ps5vkc.vkGetImageMemoryRequirements
#define vkGetPhysicalDeviceMemoryProperties ps5vkc.vkGetPhysicalDeviceMemoryProperties
#define vkGetPhysicalDeviceProperties ps5vkc.vkGetPhysicalDeviceProperties
#define vkGetQueryPoolResults ps5vkc.vkGetQueryPoolResults
#define vkMapMemory ps5vkc.vkMapMemory
#define vkUnmapMemory ps5vkc.vkUnmapMemory
#define vkUpdateDescriptorSets ps5vkc.vkUpdateDescriptorSets
#endif
