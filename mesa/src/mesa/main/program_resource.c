/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2015 Intel Corporation.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "main/enums.h"
#include "main/macros.h"
#include "main/mtypes.h"
#include "main/shaderapi.h"
#include "main/shaderobj.h"
#include "main/context.h"
#include "program_resource.h"
#include "compiler/glsl/ir_uniform.h"
static bool
supported_interface_enum(struct gl_context *ctx, GLenum iface)
{
   switch (iface) {
   case GL_UNIFORM:
   case GL_UNIFORM_BLOCK:
   case GL_PROGRAM_INPUT:
   case GL_PROGRAM_OUTPUT:
   case GL_TRANSFORM_FEEDBACK_BUFFER:
   case GL_TRANSFORM_FEEDBACK_VARYING:
   case GL_ATOMIC_COUNTER_BUFFER:
   case GL_BUFFER_VARIABLE:
   case GL_SHADER_STORAGE_BLOCK:
      return true;
   case GL_VERTEX_SUBROUTINE:
   case GL_FRAGMENT_SUBROUTINE:
   case GL_VERTEX_SUBROUTINE_UNIFORM:
   case GL_FRAGMENT_SUBROUTINE_UNIFORM:
      return _mesa_has_ARB_shader_subroutine(ctx);
   case GL_GEOMETRY_SUBROUTINE:
   case GL_GEOMETRY_SUBROUTINE_UNIFORM:
      return _mesa_has_geometry_shaders(ctx) && _mesa_has_ARB_shader_subroutine(ctx);
   case GL_COMPUTE_SUBROUTINE:
   case GL_COMPUTE_SUBROUTINE_UNIFORM:
      return _mesa_has_compute_shaders(ctx) && _mesa_has_ARB_shader_subroutine(ctx);
   case GL_TESS_CONTROL_SUBROUTINE:
   case GL_TESS_EVALUATION_SUBROUTINE:
   case GL_TESS_CONTROL_SUBROUTINE_UNIFORM:
   case GL_TESS_EVALUATION_SUBROUTINE_UNIFORM:
      return _mesa_has_tessellation(ctx) && _mesa_has_ARB_shader_subroutine(ctx);
   default:
      return false;
   }
}

static struct gl_shader_program *
lookup_linked_program(GLuint program,
                      const char *caller,
                      bool raise_link_error)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_shader_program *prog =
      _mesa_lookup_shader_program_err(ctx, program, caller);

   if (!prog)
      return NULL;

   if (prog->LinkStatus == GL_FALSE) {
      if (raise_link_error)
         _mesa_error(ctx, GL_INVALID_OPERATION, "%s(program not linked)",
                     caller);
      return NULL;
   }
   return prog;
}

static GLenum
stage_from_program_interface(GLenum programInterface)
{
   switch(programInterface) {
   case GL_VERTEX_SUBROUTINE_UNIFORM:
      return MESA_SHADER_VERTEX;
   case GL_TESS_CONTROL_SUBROUTINE_UNIFORM:
      return MESA_SHADER_TESS_CTRL;
   case GL_TESS_EVALUATION_SUBROUTINE_UNIFORM:
      return MESA_SHADER_TESS_EVAL;
   case GL_GEOMETRY_SUBROUTINE_UNIFORM:
      return MESA_SHADER_GEOMETRY;
   case GL_FRAGMENT_SUBROUTINE_UNIFORM:
      return MESA_SHADER_FRAGMENT;
   case GL_COMPUTE_SUBROUTINE_UNIFORM:
      return MESA_SHADER_COMPUTE;
   default:
      unreachable("unexpected programInterface value");
   }
}

static struct gl_linked_shader *
lookup_linked_shader(GLuint program,
                     GLenum programInterface,
                     const char *caller)
{
   struct gl_shader_program *shLinkedProg =
      lookup_linked_program(program, caller, false);
   gl_shader_stage stage = stage_from_program_interface(programInterface);

   if (!shLinkedProg)
      return NULL;

   return shLinkedProg->_LinkedShaders[stage];
}

static bool
is_subroutine_uniform_program_interface(GLenum programInterface)
{
   switch(programInterface) {
   case GL_VERTEX_SUBROUTINE_UNIFORM:
   case GL_TESS_CONTROL_SUBROUTINE_UNIFORM:
   case GL_TESS_EVALUATION_SUBROUTINE_UNIFORM:
   case GL_GEOMETRY_SUBROUTINE_UNIFORM:
   case GL_FRAGMENT_SUBROUTINE_UNIFORM:
   case GL_COMPUTE_SUBROUTINE_UNIFORM:
      return true;
   default:
      return false;
   }
}

void GLAPIENTRY
_mesa_GetProgramInterfaceiv(GLuint program, GLenum programInterface,
                            GLenum pname, GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API) {
      _mesa_debug(ctx, "glGetProgramInterfaceiv(%u, %s, %s, %p)\n",
                  program, _mesa_enum_to_string(programInterface),
                  _mesa_enum_to_string(pname), params);
   }

   unsigned i;
   struct gl_shader_program *shProg =
      _mesa_lookup_shader_program_err(ctx, program,
                                      "glGetProgramInterfaceiv");
   if (!shProg)
      return;

   if (!params) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glGetProgramInterfaceiv(params NULL)");
      return;
   }

   /* Validate interface. */
   if (!supported_interface_enum(ctx, programInterface)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, "glGetProgramInterfaceiv(%s)",
                  _mesa_enum_to_string(programInterface));
      return;
   }

   /* Validate pname against interface. */
   switch(pname) {
   case GL_ACTIVE_RESOURCES:
      if (is_subroutine_uniform_program_interface(programInterface)) {
         /* ARB_program_interface_query doesn't explicitly says that those
          * uniforms would need a linked shader, or that should fail if it is
          * not the case, but Section 7.6 (Uniform Variables) of the OpenGL
          * 4.4 Core Profile says:
          *
          *    "A uniform is considered an active uniform if the compiler and
          *     linker determine that the uniform will actually be accessed
          *     when the executable code is executed. In cases where the
          *     compiler and linker cannot make a conclusive determination,
          *     the uniform will be considered active."
          *
          * So in order to know the real number of active subroutine uniforms
          * we would need a linked shader .
          *
          * At the same time, Section 7.3 (Program Objects) of the OpenGL 4.4
          * Core Profile says:
          *
          *    "The GL provides various commands allowing applications to
          *     enumerate and query properties of active variables and in-
          *     terface blocks for a specified program. If one of these
          *     commands is called with a program for which LinkProgram
          *     succeeded, the information recorded when the program was
          *     linked is returned. If one of these commands is called with a
          *     program for which LinkProgram failed, no error is generated
          *     unless otherwise noted."
          *     <skip>
          *    "If one of these commands is called with a program for which
          *     LinkProgram had never been called, no error is generated
          *     unless otherwise noted, and the program object is considered
          *     to have no active variables or interface blocks."
          *
          * So if the program is not linked we will return 0.
          */
         struct gl_linked_shader *sh =
            lookup_linked_shader(program, programInterface, "glGetProgramInterfaceiv");

         *params = sh ? sh->NumSubroutineUniforms : 0;
      } else {
         for (i = 0, *params = 0; i < shProg->NumProgramResourceList; i++)
            if (shProg->ProgramResourceList[i].Type == programInterface)
               (*params)++;
      }
      break;
   case GL_MAX_NAME_LENGTH:
      if (programInterface == GL_ATOMIC_COUNTER_BUFFER ||
          programInterface == GL_TRANSFORM_FEEDBACK_BUFFER) {
         _mesa_error(ctx, GL_INVALID_OPERATION,
                     "glGetProgramInterfaceiv(%s pname %s)",
                     _mesa_enum_to_string(programInterface),
                     _mesa_enum_to_string(pname));
         return;
      }
      /* Name length consists of base name, 3 additional chars '[0]' if
       * resource is an array and finally 1 char for string terminator.
       */
      for (i = 0, *params = 0; i < shProg->NumProgramResourceList; i++) {
         if (shProg->ProgramResourceList[i].Type != programInterface)
            continue;
         unsigned len =
            _mesa_program_resource_name_len(&shProg->ProgramResourceList[i]);
         *params = MAX2(*params, len + 1);
      }
      break;
   case GL_MAX_NUM_ACTIVE_VARIABLES:
      switch (programInterface) {
      case GL_UNIFORM_BLOCK:
         for (i = 0, *params = 0; i < shProg->NumProgramResourceList; i++) {
            if (shProg->ProgramResourceList[i].Type == programInterface) {
               struct gl_uniform_block *block =
                  (struct gl_uniform_block *)
                  shProg->ProgramResourceList[i].Data;
               *params = MAX2(*params, block->NumUniforms);
            }
         }
         break;
      case GL_SHADER_STORAGE_BLOCK:
         for (i = 0, *params = 0; i < shProg->NumProgramResourceList; i++) {
            if (shProg->ProgramResourceList[i].Type == programInterface) {
               struct gl_uniform_block *block =
                  (struct gl_uniform_block *)
                  shProg->ProgramResourceList[i].Data;
               GLint block_params = 0;
               for (unsigned j = 0; j < block->NumUniforms; j++) {
                  const char *iname = block->Uniforms[j].IndexName;
                  struct gl_program_resource *uni =
                     _mesa_program_resource_find_name(shProg, GL_BUFFER_VARIABLE,
                                                      iname, NULL);
                  if (!uni)
                     continue;
                  block_params++;
               }
               *params = MAX2(*params, block_params);
            }
         }
         break;
      case GL_ATOMIC_COUNTER_BUFFER:
         for (i = 0, *params = 0; i < shProg->NumProgramResourceList; i++) {
            if (shProg->ProgramResourceList[i].Type == programInterface) {
               struct gl_active_atomic_buffer *buffer =
                  (struct gl_active_atomic_buffer *)
                  shProg->ProgramResourceList[i].Data;
               *params = MAX2(*params, buffer->NumUniforms);
            }
         }
         break;
      case GL_TRANSFORM_FEEDBACK_BUFFER:
         for (i = 0, *params = 0; i < shProg->NumProgramResourceList; i++) {
            if (shProg->ProgramResourceList[i].Type == programInterface) {
               struct gl_transform_feedback_buffer *buffer =
                  (struct gl_transform_feedback_buffer *)
                  shProg->ProgramResourceList[i].Data;
               *params = MAX2(*params, buffer->NumVaryings);
            }
         }
         break;
      default:
        _mesa_error(ctx, GL_INVALID_OPERATION,
                    "glGetProgramInterfaceiv(%s pname %s)",
                    _mesa_enum_to_string(programInterface),
                    _mesa_enum_to_string(pname));
      };
      break;
   case GL_MAX_NUM_COMPATIBLE_SUBROUTINES:
      switch (programInterface) {
      case GL_VERTEX_SUBROUTINE_UNIFORM:
      case GL_FRAGMENT_SUBROUTINE_UNIFORM:
      case GL_GEOMETRY_SUBROUTINE_UNIFORM:
      case GL_COMPUTE_SUBROUTINE_UNIFORM:
      case GL_TESS_CONTROL_SUBROUTINE_UNIFORM:
      case GL_TESS_EVALUATION_SUBROUTINE_UNIFORM: {
         for (i = 0, *params = 0; i < shProg->NumProgramResourceList; i++) {
            if (shProg->ProgramResourceList[i].Type == programInterface) {
               struct gl_uniform_storage *uni =
                  (struct gl_uniform_storage *)
                  shProg->ProgramResourceList[i].Data;
               *params = MAX2(*params, uni->num_compatible_subroutines);
            }
         }
         break;
      }

      default:
         _mesa_error(ctx, GL_INVALID_OPERATION,
                     "glGetProgramInterfaceiv(%s pname %s)",
                     _mesa_enum_to_string(programInterface),
                     _mesa_enum_to_string(pname));
      }
      break;
   default:
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "glGetProgramInterfaceiv(pname %s)",
                  _mesa_enum_to_string(pname));
   }
}

static bool
is_xfb_marker(const char *str)
{
   static const char *markers[] = {
      "gl_NextBuffer",
      "gl_SkipComponents1",
      "gl_SkipComponents2",
      "gl_SkipComponents3",
      "gl_SkipComponents4",
      NULL
   };
   const char **m = markers;

   if (strncmp(str, "gl_", 3) != 0)
      return false;

   for (; *m; m++)
      if (strcmp(*m, str) == 0)
         return true;

   return false;
}

GLuint GLAPIENTRY
_mesa_GetProgramResourceIndex(GLuint program, GLenum programInterface,
                              const GLchar *name)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API) {
      _mesa_debug(ctx, "glGetProgramResourceIndex(%u, %s, %s)\n",
                  program, _mesa_enum_to_string(programInterface), name);
   }

   unsigned array_index = 0;
   struct gl_program_resource *res;
   struct gl_shader_program *shProg =
      _mesa_lookup_shader_program_err(ctx, program,
                                      "glGetProgramResourceIndex");
   if (!shProg || !name)
      return GL_INVALID_INDEX;

   if (!supported_interface_enum(ctx, programInterface)) {
      _mesa_error(ctx, GL_INVALID_ENUM, "glGetProgramResourceIndex(%s)",
                  _mesa_enum_to_string(programInterface));
      return GL_INVALID_INDEX;
   }
   /*
    * For the interface TRANSFORM_FEEDBACK_VARYING, the value INVALID_INDEX
    * should be returned when querying the index assigned to the special names
    * "gl_NextBuffer", "gl_SkipComponents1", "gl_SkipComponents2",
    * "gl_SkipComponents3", and "gl_SkipComponents4".
    */
   if (programInterface == GL_TRANSFORM_FEEDBACK_VARYING &&
       is_xfb_marker(name))
      return GL_INVALID_INDEX;

   switch (programInterface) {
   case GL_TESS_CONTROL_SUBROUTINE:
   case GL_TESS_CONTROL_SUBROUTINE_UNIFORM:
   case GL_TESS_EVALUATION_SUBROUTINE:
   case GL_TESS_EVALUATION_SUBROUTINE_UNIFORM:
   case GL_COMPUTE_SUBROUTINE:
   case GL_COMPUTE_SUBROUTINE_UNIFORM:
   case GL_GEOMETRY_SUBROUTINE:
   case GL_GEOMETRY_SUBROUTINE_UNIFORM:
   case GL_VERTEX_SUBROUTINE:
   case GL_FRAGMENT_SUBROUTINE:
   case GL_VERTEX_SUBROUTINE_UNIFORM:
   case GL_FRAGMENT_SUBROUTINE_UNIFORM:
   case GL_PROGRAM_INPUT:
   case GL_PROGRAM_OUTPUT:
   case GL_UNIFORM:
   case GL_BUFFER_VARIABLE:
   case GL_TRANSFORM_FEEDBACK_VARYING:
   case GL_UNIFORM_BLOCK:
   case GL_SHADER_STORAGE_BLOCK:
      res = _mesa_program_resource_find_name(shProg, programInterface, name,
                                             &array_index);
      if (!res || array_index > 0)
         return GL_INVALID_INDEX;

      return _mesa_program_resource_index(shProg, res);
   case GL_ATOMIC_COUNTER_BUFFER:
   case GL_TRANSFORM_FEEDBACK_BUFFER:
   default:
      _mesa_error(ctx, GL_INVALID_ENUM, "glGetProgramResourceIndex(%s)",
                  _mesa_enum_to_string(programInterface));
   }

   return GL_INVALID_INDEX;
}

void GLAPIENTRY
_mesa_GetProgramResourceName(GLuint program, GLenum programInterface,
                             GLuint index, GLsizei bufSize, GLsizei *length,
                             GLchar *name)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API) {
      _mesa_debug(ctx, "glGetProgramResourceName(%u, %s, %u, %d, %p, %p)\n",
                  program, _mesa_enum_to_string(programInterface), index,
                  bufSize, length, name);
   }

   struct gl_shader_program *shProg =
      _mesa_lookup_shader_program_err(ctx, program,
                                      "glGetProgramResourceName");

   if (!shProg || !name)
      return;

   if (programInterface == GL_ATOMIC_COUNTER_BUFFER ||
       programInterface == GL_TRANSFORM_FEEDBACK_BUFFER ||
       !supported_interface_enum(ctx, programInterface)) {
      _mesa_error(ctx, GL_INVALID_ENUM, "glGetProgramResourceName(%s)",
                  _mesa_enum_to_string(programInterface));
      return;
   }

   _mesa_get_program_resource_name(shProg, programInterface, index, bufSize,
                                   length, name, "glGetProgramResourceName");
}

void GLAPIENTRY
_mesa_GetProgramResourceiv(GLuint program, GLenum programInterface,
                           GLuint index, GLsizei propCount,
                           const GLenum *props, GLsizei bufSize,
                           GLsizei *length, GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API) {
      _mesa_debug(ctx, "glGetProgramResourceiv(%u, %s, %u, %d, %p, %d, %p, %p)\n",
                  program, _mesa_enum_to_string(programInterface), index,
                  propCount, props, bufSize, length, params);
   }

   struct gl_shader_program *shProg =
      _mesa_lookup_shader_program_err(ctx, program, "glGetProgramResourceiv");

   if (!shProg || !params)
      return;

   /* The error INVALID_VALUE is generated if <propCount> is zero.
    * Note that we check < 0 here because it makes sense to bail early.
    */
   if (propCount <= 0) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetProgramResourceiv(propCount <= 0)");
      return;
   }

   _mesa_get_program_resourceiv(shProg, programInterface, index,
                                propCount, props, bufSize, length, params);
}

GLint GLAPIENTRY
_mesa_GetProgramResourceLocation(GLuint program, GLenum programInterface,
                                 const GLchar *name)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API) {
      _mesa_debug(ctx, "glGetProgramResourceLocation(%u, %s, %s)\n",
                  program, _mesa_enum_to_string(programInterface), name);
   }

   struct gl_shader_program *shProg =
      lookup_linked_program(program, "glGetProgramResourceLocation", true);

   if (!shProg || !name)
      return -1;

   /* Validate programInterface. */
   switch (programInterface) {
   case GL_UNIFORM:
   case GL_PROGRAM_INPUT:
   case GL_PROGRAM_OUTPUT:
      break;

   case GL_VERTEX_SUBROUTINE_UNIFORM:
   case GL_FRAGMENT_SUBROUTINE_UNIFORM:
      if (!_mesa_has_ARB_shader_subroutine(ctx))
         goto fail;
      break;
   case GL_GEOMETRY_SUBROUTINE_UNIFORM:
      if (!_mesa_has_geometry_shaders(ctx) || !_mesa_has_ARB_shader_subroutine(ctx))
         goto fail;
      break;
   case GL_COMPUTE_SUBROUTINE_UNIFORM:
      if (!_mesa_has_compute_shaders(ctx) || !_mesa_has_ARB_shader_subroutine(ctx))
         goto fail;
      break;
   case GL_TESS_CONTROL_SUBROUTINE_UNIFORM:
   case GL_TESS_EVALUATION_SUBROUTINE_UNIFORM:
      if (!_mesa_has_tessellation(ctx) || !_mesa_has_ARB_shader_subroutine(ctx))
         goto fail;
      break;
   default:
         goto fail;
   }

   return _mesa_program_resource_location(shProg, programInterface, name);
fail:
   _mesa_error(ctx, GL_INVALID_ENUM, "glGetProgramResourceLocation(%s %s)",
               _mesa_enum_to_string(programInterface), name);
   return -1;
}

/**
 * Returns output index for dual source blending.
 */
GLint GLAPIENTRY
_mesa_GetProgramResourceLocationIndex(GLuint program, GLenum programInterface,
                                      const GLchar *name)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_API) {
      _mesa_debug(ctx, "glGetProgramResourceLocationIndex(%u, %s, %s)\n",
                  program, _mesa_enum_to_string(programInterface), name);
   }

   struct gl_shader_program *shProg =
      lookup_linked_program(program, "glGetProgramResourceLocationIndex", true);

   if (!shProg || !name)
      return -1;

   /* From the GL_ARB_program_interface_query spec:
    *
    * "For GetProgramResourceLocationIndex, <programInterface> must be
    * PROGRAM_OUTPUT."
    */
   if (programInterface != GL_PROGRAM_OUTPUT) {
      _mesa_error(ctx, GL_INVALID_ENUM,
                  "glGetProgramResourceLocationIndex(%s)",
                  _mesa_enum_to_string(programInterface));
      return -1;
   }

   return _mesa_program_resource_location_index(shProg, GL_PROGRAM_OUTPUT,
                                                name);
}